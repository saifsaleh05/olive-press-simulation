/*
 * malaxation.c - Malaxation process (owner: Ibrahim).
 *
 * Slowly mixes the olive paste so small oil droplets join into larger ones.
 *
 *   Crushing --[Batch]--> Malaxation --[Batch]--> Separation
 *                             |
 *                             +--[MachineEvent]--> Quality Control
 *
 * - Mixing time = MALAXATION_BASE_TIME * (batch kg / NOMINAL_BATCH_KG).
 * - Temperature/pressure follow the shared model (common/simulation.c).
 * - Malaxing above MALAXATION_MAX_TEMPERATURE damages the paste: the batch
 *   quality drops by QUALITY_LOSS_RATE per second, which later lowers the
 *   extraction efficiency in Separation.
 * - The paste mass does not change here (no water is added).
 * - Pause / failure: progress freezes, the batch stays in this process and
 *   continues from the same point after SIG_RESUME, so no batch is lost.
 * - When Crushing closes its pipe (EOF) and the last batch has been passed
 *   on, Malaxation closes its output pipe, which gives Separation its EOF.
 */
#define _POSIX_C_SOURCE 200809L
#include "processes.h"
#include "simulation.h"
#include "ipc.h"

#include <stdio.h>

typedef struct {
    int out_fd;
} MalaxCtx;

static void malax_tick(Machine *m, Batch *b, double dt, void *ctx)
{
    (void)ctx;
    if (m->temperature > m->mc->max_temperature) {
        b->quality -= m->cfg->quality_loss_rate * dt;
        if (b->quality < 0.0)
            b->quality = 0.0;
    }
}

static int malax_done(Machine *m, Batch *b, void *ctx)
{
    MalaxCtx *c = ctx;

    printf("%s[malaxation] batch %d mixed: %.2f kg paste, %.1fs, T=%.1fC, "
           "quality %.0f%% -> separation\n",
           log_stamp(), b->id, b->current_mass, m->batch_time,
           m->temperature, b->quality * 100.0);

    /* Blocks while Separation's input is full: that is the back-pressure. */
    if (write_full(c->out_fd, b, sizeof *b) < 0) {
        perror("malaxation: write to separation");
        return -1;
    }
    return 0;
}

int malaxation_process(const Config *cfg, int in_fd, int out_fd, int telem_fd)
{
    Machine m;
    MalaxCtx ctx = { .out_fd = out_fd };
    unsigned int n;

    setvbuf(stdout, NULL, _IOLBF, 0);
    machine_install_signals();
    machine_init(&m, M_MALAXATION, cfg, telem_fd);
    printf("%s[malaxation] started (pid %d)\n", log_stamp(), (int)m.pid);

    n = machine_run_stage(&m, in_fd, malax_tick, malax_done, &ctx);

    printf("%s[malaxation] finished: %u batches mixed\n", log_stamp(), n);
    close_fd(&in_fd);
    close_fd(&out_fd);          /* -> EOF for Separation */
    close_fd(&m.telem_fd);
    return 0;
}
