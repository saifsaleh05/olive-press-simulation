/* Cleaning: remove the configured debris fraction and forward every batch. */
#define _POSIX_C_SOURCE 200809L
#include "processes.h"
#include "simulation.h"
#include "ipc.h"

#include <stdio.h>

enum { CLEANING_LOSS_STREAM = 0x434C4541u };
typedef struct { int out_fd; } CleaningCtx;

static double clamp01(double value)
{
    return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}

double cleaning_apply_loss(Batch *batch, const Config *cfg, double loss_u)
{
    double before, fraction, removed, impurity_mass;

    if (!batch || !cfg)
        return 0.0;
    before = batch->current_mass > 0.0 ? batch->current_mass : 0.0;
    fraction = cfg->cleaning_loss.min + clamp01(loss_u) *
               (cfg->cleaning_loss.max - cfg->cleaning_loss.min);
    fraction = clamp01(fraction);
    removed = before * fraction;
    impurity_mass = before * clamp01(batch->impurity_fraction) - removed;
    if (impurity_mass < 0.0)
        impurity_mass = 0.0;
    batch->current_mass = before - removed;
    batch->impurity_fraction = batch->current_mass > 0.0
        ? impurity_mass / batch->current_mass : 0.0;
    return removed;
}

static int cleaning_done(Machine *machine, Batch *batch, void *ctx)
{
    CleaningCtx *cleaning = ctx;
    double before = batch->current_mass;
    double unit = simulation_random_unit(machine->cfg->seed,
                                         CLEANING_LOSS_STREAM,
                                         (unsigned int)batch->id);
    double removed = cleaning_apply_loss(batch, machine->cfg, unit);

    printf("%s[cleaning] batch %d: %.2f -> %.2f kg, removed %.2f kg "
           "(%.2f%%), impurity %.2f%% -> washing\n",
           log_stamp(), batch->id, before, batch->current_mass, removed,
           before > 0.0 ? removed * 100.0 / before : 0.0,
           batch->impurity_fraction * 100.0);
    if (write_full(cleaning->out_fd, batch, sizeof *batch) < 0) {
        perror("cleaning: write to washing");
        return -1;
    }
    return 0;
}

int cleaning_process(const Config *cfg, int in_fd, int out_fd, int telem_fd)
{
    Machine machine;
    CleaningCtx ctx = { .out_fd = out_fd };
    unsigned int completed;

    setvbuf(stdout, NULL, _IOLBF, 0);
    machine_install_signals();
    machine_init(&machine, M_CLEANING, cfg, telem_fd);
    printf("%s[cleaning] started (pid %d)\n", log_stamp(), (int)machine.pid);
    completed = machine_run_stage(&machine, in_fd, NULL, cleaning_done, &ctx);
    printf("%s[cleaning] finished: %u batches cleaned\n",
           log_stamp(), completed);
    close_fd(&in_fd);
    close_fd(&out_fd);
    close_fd(&machine.telem_fd);
    return 0;
}
