/* Crushing: turn washed olives (including pits) into mass-conserving paste. */
#define _POSIX_C_SOURCE 200809L
#include "processes.h"
#include "simulation.h"
#include "ipc.h"

#include <stdio.h>

typedef struct {
    int out_fd;
} CrushingCtx;

static int crushing_done(Machine *m, Batch *b, void *ctx)
{
    CrushingCtx *c = ctx;

    /* Batch has no material-form field in the shared wire contract. The stage
     * value is the agreed marker that current_mass now represents paste. */
    printf("%s[crushing] batch %d: %.2f kg olives -> paste in %.2fs "
           "(load %.2f), T=%.1fC -> malaxation\n",
           log_stamp(), b->id, b->current_mass, m->batch_time, m->load,
           m->temperature);
    if (write_full(c->out_fd, b, sizeof *b) < 0) {
        perror("crushing: write to malaxation");
        return -1;
    }
    return 0;
}

int crushing_process(const Config *cfg, int in_fd, int out_fd, int telem_fd)
{
    Machine m;
    CrushingCtx ctx = { .out_fd = out_fd };
    unsigned int n;

    setvbuf(stdout, NULL, _IOLBF, 0);
    machine_install_signals();
    machine_init(&m, M_CRUSHING, cfg, telem_fd);
    printf("%s[crushing] started (pid %d)\n", log_stamp(), (int)m.pid);

    n = machine_run_stage(&m, in_fd, NULL, crushing_done, &ctx);

    printf("%s[crushing] finished: %u batches crushed\n", log_stamp(), n);
    close_fd(&in_fd);
    close_fd(&out_fd);
    close_fd(&m.telem_fd);
    return 0;
}
