#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "config.h"
#include "ipc.h"
#include "processes.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
    failures++; } } while (0)

static int child_ok(pid_t pid)
{
    int status;
    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}

int main(void)
{
    Config cfg;
    int first[2], second[2];
    pid_t reception, cleaning;
    int seen[128] = {0};
    int count = 0;
    double original = 0.0, cleaned = 0.0;

    config_defaults(&cfg);
    cfg.total_olives_kg = 65.0;
    cfg.batch_size_kg = (Range){10.0, 14.0};
    cfg.nominal_batch_kg = 10.0;
    cfg.tick_ms = 1;
    cfg.machine[M_RECEPTION].base_time = 0.001;
    cfg.machine[M_CLEANING].base_time = 0.001;
    cfg.auto_faults_enabled = 0;
    cfg.seed = 4330;

    CHECK(pipe(first) == 0);
    CHECK(pipe(second) == 0);
    reception = fork();
    CHECK(reception >= 0);
    if (reception == 0) {
        close(first[0]); close(second[0]); close(second[1]);
        _exit(reception_process(&cfg, first[1], -1));
    }
    cleaning = fork();
    CHECK(cleaning >= 0);
    if (cleaning == 0) {
        close(first[1]); close(second[0]);
        _exit(cleaning_process(&cfg, first[0], second[1], -1));
    }

    close(first[0]); close(first[1]); close(second[1]);
    for (;;) {
        Batch batch;
        ssize_t bytes = read_full(second[0], &batch, sizeof batch);
        if (bytes == 0)
            break;
        CHECK(bytes == (ssize_t)sizeof batch);
        if (bytes != (ssize_t)sizeof batch)
            break;
        CHECK(batch.id > 0 && batch.id < 128);
        CHECK(!seen[batch.id]);
        if (batch.id > 0 && batch.id < 128)
            seen[batch.id] = 1;
        CHECK(batch.stage == M_CLEANING);
        CHECK(batch.current_mass < batch.original_mass);
        CHECK(batch.impurity_fraction >= 0.0 && batch.impurity_fraction <= 1.0);
        original += batch.original_mass;
        cleaned += batch.current_mass;
        count++;
    }
    close(second[0]);
    CHECK(child_ok(reception));
    CHECK(child_ok(cleaning));
    CHECK(count >= 2);
    CHECK(fabs(original - cfg.total_olives_kg) < 1e-9);
    CHECK(cleaned < original);

    if (failures) {
        fprintf(stderr, "test_reception_cleaning: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_reception_cleaning: ok (%d unique batches, %.2f -> %.2f kg)\n",
           count, original, cleaned);
    return 0;
}
