#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "simulation.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static int near(double a, double b)
{
    return fabs(a - b) < 1e-9;
}

static int rejects(Config *cfg, const char *text)
{
    char path[] = "/tmp/olive-config-XXXXXX";
    int fd = mkstemp(path);
    FILE *f;
    int rc;

    if (fd < 0)
        return 0;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(path);
        return 0;
    }
    fputs(text, f);
    fclose(f);
    rc = config_load(cfg, path);
    unlink(path);
    return rc == -1;
}

int main(int argc, char **argv)
{
    Config cfg, before;
    Machine hot, same, other;
    char err[128];
    double a, b;

    config_defaults(&cfg);
    if (argc > 1)
        CHECK(config_load(&cfg, argv[1]) == 0, "valid config loads");
    CHECK(config_validate(&cfg, err, sizeof err) == 0, "defaults validate");

    machine_init(&hot, M_CRUSHING, &cfg, -1);
    hot.state = ST_OPERATING;
    hot.temperature = cfg.ambient_temperature;
    hot.load = 1.0;
    machine_physics_step(&hot, 0.5);
    CHECK(near(hot.temperature,
               cfg.ambient_temperature + cfg.machine[M_CRUSHING].heating_rate * 0.5),
          "operating machine heats from ambient");

    hot.state = ST_IDLE;
    hot.temperature = cfg.ambient_temperature + 10.0;
    machine_physics_step(&hot, 1.0);
    CHECK(near(hot.temperature, cfg.ambient_temperature + 10.0 -
               cfg.machine[M_CRUSHING].cooling_factor * 10.0),
          "idle machine cools toward ambient");

    CHECK(near(machine_processing_time(&cfg, M_CRUSHING,
                                       cfg.nominal_batch_kg),
               cfg.machine[M_CRUSHING].base_time),
          "nominal weight uses base duration");
    CHECK(near(machine_processing_time(&cfg, M_CRUSHING,
                                       cfg.nominal_batch_kg * 2.0),
               cfg.machine[M_CRUSHING].base_time * 2.0),
          "double weight doubles duration");

    machine_init(&same, M_CRUSHING, &cfg, -1);
    machine_init(&other, M_WASHING, &cfg, -1);
    a = machine_random(&hot);
    b = machine_random(&same);
    /* Re-initialize hot: physics does not consume random values. */
    machine_init(&hot, M_CRUSHING, &cfg, -1);
    CHECK(near(machine_random(&hot), b), "fixed seed and machine reproduce stream");
    CHECK(!near(b, machine_random(&other)), "machine streams are distinct");
    CHECK(near(simulation_random_unit(cfg.seed, 7, 99),
               simulation_random_unit(cfg.seed, 7, 99)),
          "stateless sample is reproducible");
    (void)a;

    before = cfg;
    CHECK(rejects(&cfg, "BATCH_SIZE_MIN=40\nBATCH_SIZE_MAX=20\n"),
          "loader rejects inverted range");
    CHECK(memcmp(&cfg, &before, sizeof cfg) == 0,
          "failed load leaves config unchanged");
    CHECK(rejects(&cfg, "TOTAL_OLIVES_KG=-1\n"),
          "loader rejects negative quantity");
    CHECK(rejects(&cfg, "QUEUE_CAPACITY=2.5\n"),
          "loader rejects fractional integer");
    CHECK(rejects(&cfg, "TEMPERATURE_NOISE=nan\n"),
          "loader rejects non-finite value");

    if (failures) {
        fprintf(stderr, "test_config_simulation: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_config_simulation: PASS\n");
    return 0;
}
