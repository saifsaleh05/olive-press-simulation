/* Washing: remove the configured fraction of soil/impurities and forward. */
#define _POSIX_C_SOURCE 200809L
#include "processes.h"
#include "simulation.h"
#include "ipc.h"

#include <stdio.h>

enum { WASHING_LOSS_STREAM = 0x57415348u }; /* "WASH" */

typedef struct {
    int out_fd;
} WashingCtx;

static double clamp01(double x)
{
    return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}

double washing_apply_loss(Batch *b, const Config *cfg, double loss_u)
{
    double before, fraction, removed, impurity_mass;

    if (!b || !cfg)
        return 0.0;
    before = b->current_mass > 0.0 ? b->current_mass : 0.0;
    fraction = cfg->washing_loss.min + clamp01(loss_u) *
               (cfg->washing_loss.max - cfg->washing_loss.min);
    fraction = clamp01(fraction);
    removed = before * fraction;

    /* Washing removes impurity first. Any configured excess represents small
     * olive/material loss. Store the remaining impurity as a fraction of the
     * remaining batch, not of its old mass. */
    impurity_mass = before * clamp01(b->impurity_fraction) - removed;
    if (impurity_mass < 0.0)
        impurity_mass = 0.0;
    b->current_mass = before - removed;
    b->impurity_fraction = b->current_mass > 0.0
                         ? impurity_mass / b->current_mass : 0.0;
    return removed;
}

static int washing_done(Machine *m, Batch *b, void *ctx)
{
    WashingCtx *c = ctx;
    double before = b->current_mass;
    double u = simulation_random_unit(m->cfg->seed, WASHING_LOSS_STREAM,
                                      (unsigned int)b->id);
    double removed = washing_apply_loss(b, m->cfg, u);

    printf("%s[washing] batch %d: %.2f kg -> %.2f kg, removed %.2f kg "
           "(%.2f%%), impurity %.2f%% -> crushing\n",
           log_stamp(), b->id, before, b->current_mass, removed,
           before > 0.0 ? 100.0 * removed / before : 0.0,
           b->impurity_fraction * 100.0);
    if (write_full(c->out_fd, b, sizeof *b) < 0) {
        perror("washing: write to crushing");
        return -1;
    }
    return 0;
}

int washing_process(const Config *cfg, int in_fd, int out_fd, int telem_fd)
{
    Machine m;
    WashingCtx ctx = { .out_fd = out_fd };
    unsigned int n;

    setvbuf(stdout, NULL, _IOLBF, 0);
    machine_install_signals();
    machine_init(&m, M_WASHING, cfg, telem_fd);
    printf("%s[washing] started (pid %d)\n", log_stamp(), (int)m.pid);

    n = machine_run_stage(&m, in_fd, NULL, washing_done, &ctx);

    printf("%s[washing] finished: %u batches washed\n", log_stamp(), n);
    close_fd(&in_fd);
    close_fd(&out_fd);
    close_fd(&m.telem_fd);
    return 0;
}
