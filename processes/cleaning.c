#include "cleaning.h"

#include "common.h"
#include "ipc.h"
#include "timing.h"

#include <limits.h>
#include <stdio.h>
#include <unistd.h>

static unsigned int next_random(unsigned int *state)
{
    unsigned int value = *state;
    if (value == 0U) {
        value = 0x9e3779b9U;
    }
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static double sample_range(unsigned int *state, double minimum, double maximum)
{
    double unit = (double)next_random(state) / (double)UINT_MAX;
    return minimum + ((maximum - minimum) * unit);
}

int run_cleaning(const Config *config, int input_fd, int output_fd)
{
    unsigned int random_state = config->seed ^ 0x434c4541U;

    for (;;) {
        Batch batch;
        ssize_t received = read_full(input_fd, &batch, sizeof(batch));
        double efficiency;
        double removed;

        if (received == 0) {
            fprintf(stdout,
                    "[Cleaning pid=%ld] Reception closed; all queued batches drained\n",
                    (long)getpid());
            return 0;
        }
        if (received == -1) {
            perror("Cleaning: read batch");
            return 1;
        }

        efficiency = sample_range(&random_state,
                                  config->cleaning_efficiency_min,
                                  config->cleaning_efficiency_max);
        if (sleep_milliseconds(config->cleaning_duration_ms) == -1) {
            perror("Cleaning: processing delay");
            return 1;
        }
        removed = batch.impurity_mass_kg * efficiency;
        batch.impurity_mass_kg -= removed;
        batch.removed_impurity_kg += removed;
        batch.current_mass_kg -= removed;
        batch.quality = batch.current_mass_kg > 0.0
                            ? batch.olive_mass_kg / batch.current_mass_kg
                            : 0.0;
        batch.stage = STAGE_CLEANING;

        fprintf(stdout,
                "[Cleaning] Batch %d: removed %.2f kg (%.1f%%); %.2f kg remains\n",
                batch.id, removed, efficiency * 100.0,
                batch.current_mass_kg);

        if (write_full(output_fd, &batch, sizeof(batch)) == -1) {
            perror("Cleaning: write batch");
            return 1;
        }
    }
}
