/* Reception: split the configured container mass into deterministic batches. */
#define _POSIX_C_SOURCE 200809L
#include "processes.h"
#include "simulation.h"
#include "ipc.h"

#include <stdio.h>

enum {
    RECEPTION_MASS_STREAM = 0x4D415353u,
    RECEPTION_IMPURITY_STREAM = 0x494D5055u,
    RECEPTION_OIL_STREAM = 0x4F494C50u,
    RECEPTION_QUALITY_STREAM = 0x5155414Cu
};

typedef struct { int out_fd; } ReceptionCtx;

static double in_range(Range range, double unit)
{
    return range.min + unit * (range.max - range.min);
}

static int reception_done(Machine *machine, Batch *batch, void *ctx)
{
    ReceptionCtx *reception = ctx;

    if (write_full(reception->out_fd, batch, sizeof *batch) < 0) {
        perror("reception: write to cleaning");
        return -1;
    }
    printf("%s[reception] batch %d: %.2f kg, impurity %.2f%% -> cleaning\n",
           log_stamp(), batch->id, batch->current_mass,
           batch->impurity_fraction * 100.0);
    (void)machine;
    return 0;
}

int reception_process(const Config *cfg, int out_fd, int telem_fd)
{
    Machine machine;
    ReceptionCtx ctx = { .out_fd = out_fd };
    double remaining = cfg->total_olives_kg;
    int id = 1;
    int failed = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    machine_install_signals();
    machine_init(&machine, M_RECEPTION, cfg, telem_fd);
    machine_emit(&machine, EV_STATE_CHANGE);
    printf("%s[reception] conveyor started: %.2f kg available\n",
           log_stamp(), remaining);

    while (remaining > 1e-9 && !machine_stop_requested()) {
        Batch batch;
        unsigned int sample = (unsigned int)id;
        double mass = in_range(cfg->batch_size_kg,
            simulation_random_unit(cfg->seed, RECEPTION_MASS_STREAM, sample));

        if (mass > remaining)
            mass = remaining;
        batch.id = id;
        batch.original_mass = mass;
        batch.current_mass = mass;
        batch.impurity_fraction = in_range(cfg->impurity,
            simulation_random_unit(cfg->seed, RECEPTION_IMPURITY_STREAM, sample));
        batch.oil_potential = in_range(cfg->oil_potential,
            simulation_random_unit(cfg->seed, RECEPTION_OIL_STREAM, sample));
        batch.quality = in_range(cfg->quality,
            simulation_random_unit(cfg->seed, RECEPTION_QUALITY_STREAM, sample));
        batch.stage = M_RECEPTION;

        if (machine_run_source_batch(&machine, &batch, NULL,
                                     reception_done, &ctx) < 0) {
            failed = 1;
            break;
        }
        remaining -= mass;
        if (remaining < 1e-9)
            remaining = 0.0;
        id++;
    }

    machine.state = (!failed && remaining <= 1e-9) ? ST_COMPLETED : ST_IDLE;
    machine.batch_id = -1;
    machine.progress = 0.0;
    machine_emit(&machine, EV_STATE_CHANGE);
    machine_emit(&machine, EV_SHUTDOWN);
    printf("%s[reception] conveyor stopped: %u batches, %.2f kg remaining\n",
           log_stamp(), machine.batches_done, remaining);
    close_fd(&out_fd);
    close_fd(&machine.telem_fd);
    return failed ? 1 : 0;
}
