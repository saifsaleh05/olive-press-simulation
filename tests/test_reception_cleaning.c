#include "cleaning.h"
#include "common.h"
#include "config.h"
#include "ipc.h"
#include "reception.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_TEST_BATCHES 64

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(EXIT_FAILURE);
}

static void expect_clean_exit(pid_t pid, const char *name)
{
    int status;
    if (waitpid(pid, &status, 0) != pid) {
        fail("waitpid failed");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: %s did not exit cleanly\n", name);
        exit(EXIT_FAILURE);
    }
}

int main(void)
{
    Config config;
    IpcPipe pipes[2];
    pid_t reception_pid;
    pid_t cleaning_pid;
    int seen[MAX_TEST_BATCHES] = {0};
    int batch_count = 0;
    double original_total = 0.0;
    double cleaned_total = 0.0;

    config_set_defaults(&config);
    config.total_olive_mass_kg = 425.0;
    config.batch_mass_min_kg = 60.0;
    config.batch_mass_max_kg = 90.0;
    config.reception_interval_ms = 0U;
    config.cleaning_duration_ms = 0U;
    config.seed = 12345U;

    if (ipc_create_pipes(pipes, 2) == -1) {
        fail("could not create test pipes");
    }

    reception_pid = fork();
    if (reception_pid == -1) {
        fail("could not fork Reception");
    }
    if (reception_pid == 0) {
        int keep = pipes[0].write_fd;
        int result;
        ipc_close_all_except(pipes, 2, &keep, 1);
        result = run_reception(&config, keep);
        ipc_close_all(pipes, 2);
        _exit(result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    cleaning_pid = fork();
    if (cleaning_pid == -1) {
        fail("could not fork Cleaning");
    }
    if (cleaning_pid == 0) {
        int keep[2] = {pipes[0].read_fd, pipes[1].write_fd};
        int result;
        ipc_close_all_except(pipes, 2, keep, 2);
        result = run_cleaning(&config, keep[0], keep[1]);
        ipc_close_all(pipes, 2);
        _exit(result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    ipc_close_fd(&pipes[0].read_fd);
    ipc_close_fd(&pipes[0].write_fd);
    ipc_close_fd(&pipes[1].write_fd);

    for (;;) {
        Batch batch;
        ssize_t received = read_full(pipes[1].read_fd, &batch, sizeof(batch));
        double reconstructed_mass;

        if (received == 0) {
            break;
        }
        if (received == -1) {
            fail("mock Washing could not read a complete batch");
        }
        if (batch.id <= 0 || batch.id >= MAX_TEST_BATCHES) {
            fail("batch ID is outside the test range");
        }
        if (seen[batch.id]) {
            fail("duplicate batch ID received");
        }
        seen[batch.id] = 1;
        ++batch_count;

        if (batch.stage != STAGE_CLEANING) {
            fail("batch was not marked as cleaned");
        }
        if (batch.removed_impurity_kg <= 0.0) {
            fail("Cleaning removed no impurity");
        }
        if (batch.current_mass_kg >= batch.original_mass_kg) {
            fail("cleaned mass did not decrease");
        }
        reconstructed_mass = batch.olive_mass_kg + batch.impurity_mass_kg;
        if (fabs(reconstructed_mass - batch.current_mass_kg) > 0.000001) {
            fail("mass accounting invariant failed");
        }
        original_total += batch.original_mass_kg;
        cleaned_total += batch.current_mass_kg;
        printf("[Mock Washing] Batch %d received once: %.2f kg\n",
               batch.id, batch.current_mass_kg);
    }

    ipc_close_fd(&pipes[1].read_fd);
    expect_clean_exit(reception_pid, "Reception");
    expect_clean_exit(cleaning_pid, "Cleaning");

    if (batch_count < 2) {
        fail("test did not transfer multiple batches");
    }
    if (fabs(original_total - config.total_olive_mass_kg) > 0.000001) {
        fail("batches did not cover the configured container mass");
    }
    if (cleaned_total >= original_total) {
        fail("total cleaning output should be lighter than input");
    }

    printf("PASS: %d unique batches, no loss/duplicates, EOF propagated; "
           "%.2f kg -> %.2f kg\n",
           batch_count, original_total, cleaned_total);
    return EXIT_SUCCESS;
}
