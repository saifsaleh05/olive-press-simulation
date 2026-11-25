#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "config.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static const char *write_tmp(const char *body)
{
    static char path[64];
    FILE *fp;

    snprintf(path, sizeof path, "/tmp/olive_cfg_%d.cfg", (int)getpid());
    fp = fopen(path, "w");
    fputs(body, fp);
    fclose(fp);
    return path;
}

static int load(const char *body, Config *c, char *err)
{
    config_defaults(c);
    return config_load(write_tmp(body), c, err, 256);
}

int main(void)
{
    Config c;
    char   err[256];

    config_defaults(&c);
    CHECK(config_validate(&c, err, sizeof err) == 0);

    CHECK(load("# only comments\n\n   \n", &c, err) == 0);
    CHECK(c.num_batches == 10);

    CHECK(load("seed = 99\nnum_batches=3  # inline comment\n"
               "crushing.max_temp = 41.5\nambient_temp = 18\n", &c, err) == 0);
    CHECK(c.seed == 99);
    CHECK(c.num_batches == 3);
    CHECK(c.m[M_CRUSHING].max_temp == 41.5);
    CHECK(c.ambient_temp == 18.0);
    CHECK(c.m[M_MALAXATION].max_temp == 27.0);     /* untouched default */

    CHECK(load("crushng.max_temp = 40\n", &c, err) < 0);
    CHECK(strstr(err, "unknown key") != NULL);

    CHECK(load("num_batches = lots\n", &c, err) < 0);
    CHECK(strstr(err, "bad number") != NULL);

    CHECK(load("just some words\n", &c, err) < 0);

    CHECK(load("batch_mass_min = 700\nbatch_mass_max = 500\n", &c, err) < 0);
    CHECK(load("oil_potential_max = 0.4\nwater_fraction_max = 0.7\n", &c, err) < 0);
    CHECK(load("ambient_temp = 30\n", &c, err) < 0);   /* above malaxation limit */

    config_defaults(&c);
    CHECK(config_load("/does/not/exist.cfg", &c, err, sizeof err) < 0);

    CHECK(load("", &c, err) == 0);
    CHECK(config_load("config/default.cfg", &c, err, sizeof err) == 0);
    CHECK(config_load("config/hot_day.cfg", &c, err, sizeof err) == 0);

    unlink(write_tmp(""));

    if (failures) {
        fprintf(stderr, "test_config: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_config: ok\n");
    return 0;
}
