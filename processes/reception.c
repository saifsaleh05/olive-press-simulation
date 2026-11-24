#include "reception.h"

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
        value = 0x6d2b79f5U;
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

int run_reception(const Config *config, int output_fd)
{
    unsigned int random_state = config->seed;
    double remaining = config->total_olive_mass_kg;
    int next_batch_id = 1;

    fprintf(stdout, "[Reception pid=%ld] Conveyor started; container has %.2f kg\n",
            (long)getpid(), remaining);

    while (remaining > 0.000001) {
        Batch batch;
        double requested_mass = sample_range(&random_state,
                                             config->batch_mass_min_kg,
                                             config->batch_mass_max_kg);
        double impurity_fraction;

        if (requested_mass > remaining) {
            requested_mass = remaining;
        }
        impurity_fraction = sample_range(&random_state,
                                         config->impurity_fraction_min,
                                         config->impurity_fraction_max);

        batch.id = next_batch_id++;
        batch.original_mass_kg = requested_mass;
        batch.current_mass_kg = requested_mass;
        batch.impurity_mass_kg = requested_mass * impurity_fraction;
        batch.olive_mass_kg = requested_mass - batch.impurity_mass_kg;
        batch.removed_impurity_kg = 0.0;
        batch.oil_potential_fraction = sample_range(&random_state,
                                                    config->oil_potential_min,
                                                    config->oil_potential_max);
        batch.quality = 1.0 - impurity_fraction;
        batch.stage = STAGE_RECEPTION;

        if (sleep_milliseconds(config->reception_interval_ms) == -1) {
            perror("Reception: processing delay");
            return 1;
        }
        if (write_full(output_fd, &batch, sizeof(batch)) == -1) {
            perror("Reception: write batch");
            return 1;
        }

        remaining -= requested_mass;
        if (remaining < 0.000001) {
            remaining = 0.0;
        }
        fprintf(stdout,
                "[Reception] Batch %d: %.2f kg (%.2f kg impurity); container %.2f kg\n",
                batch.id, batch.current_mass_kg, batch.impurity_mass_kg,
                remaining);
    }

    fprintf(stdout, "[Reception] Container empty; conveyor stopped; closing output\n");
    return 0;
}
