/*
 * separation.c - Pressing / Separation process (owner: Ibrahim).
 *
 * Three-phase decanter: splits the malaxed paste into olive oil, vegetable
 * water and solid pomace.
 *
 *   Malaxation --[Batch]--> Separation --[ProductionResult]--> Supervisor
 *                               |
 *                               +--[MachineEvent]--> Quality Control
 *
 * Mass balance (all in kg):
 *   paste      = batch.current_mass   (olives minus Cleaning/Washing losses)
 *   losses     = batch.original_mass - paste
 *   efficiency = EXTRACTION_EFFICIENCY * (1 - QUALITY_WEIGHT
 *                                         + QUALITY_WEIGHT * batch.quality)
 *   oil        = paste * oil_potential * efficiency
 *   water      = paste * veg_water,  veg_water drawn from VEG_WATER_PERCENT
 *   pomace     = paste - oil - water  (the oil that was not recovered stays
 *                                      in the pomace, nothing disappears)
 * => paste == oil + water + pomace and
 *    original_mass == losses + oil + water + pomace.
 */
#define _POSIX_C_SOURCE 200809L
#include "processes.h"
#include "simulation.h"
#include "ipc.h"

#include <stdio.h>
#include <string.h>

enum { SEPARATION_WATER_STREAM = 0x57415452u }; /* "WATR" */

static double clamp01(double x)
{
    return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}

void separation_compute(const Batch *b, const Config *cfg, double water_u,
                        ProductionResult *r)
{
    double paste = b->current_mass > 0.0 ? b->current_mass : 0.0;
    double orig  = b->original_mass > paste ? b->original_mass : paste;
    double w     = clamp01(cfg->quality_weight);
    double eff   = clamp01(cfg->extraction_efficiency) *
                   (1.0 - w + w * clamp01(b->quality));
    double vw    = cfg->veg_water.min +
                   clamp01(water_u) * (cfg->veg_water.max - cfg->veg_water.min);
    double oil, water;

    oil   = paste * clamp01(b->oil_potential) * eff;
    water = paste * clamp01(vw);
    if (oil + water > paste)            /* impossible config: no negative pomace */
        water = paste - oil;

    memset(r, 0, sizeof *r);
    r->batch_id      = b->id;
    r->original_mass = orig;
    r->input_mass    = paste;
    r->losses        = orig - paste;
    r->oil           = oil;
    r->water         = water;
    r->pomace        = paste - oil - water;
    r->quality       = clamp01(b->quality);
}

typedef struct {
    int    result_fd;
    double oil, water, pomace, input, losses;
} SepCtx;

static int sep_done(Machine *m, Batch *b, void *ctx)
{
    SepCtx *c = ctx;
    ProductionResult r;

    /* Production must depend on seed + batch id, not on how many telemetry
     * samples the scheduler happened to emit before this batch completed. */
    separation_compute(b, m->cfg,
                       simulation_random_unit(m->cfg->seed,
                                              SEPARATION_WATER_STREAM,
                                              (unsigned int)b->id),
                       &r);
    c->oil    += r.oil;
    c->water  += r.water;
    c->pomace += r.pomace;
    c->input  += r.input_mass;
    c->losses += r.losses;

    printf("%s[separation] batch %d: %.2f kg paste -> oil %.2f kg (%.1f%%), "
           "water %.2f kg, pomace %.2f kg | losses before %.2f kg\n",
           log_stamp(), r.batch_id, r.input_mass, r.oil,
           r.input_mass > 0 ? 100.0 * r.oil / r.input_mass : 0.0,
           r.water, r.pomace, r.losses);

    if (c->result_fd >= 0 && write_full(c->result_fd, &r, sizeof r) < 0) {
        perror("separation: write result to supervisor");
        return -1;
    }
    return 0;
}

int separation_process(const Config *cfg, int in_fd, int result_fd,
                       int telem_fd)
{
    Machine m;
    SepCtx ctx;
    unsigned int n;

    memset(&ctx, 0, sizeof ctx);
    ctx.result_fd = result_fd;

    setvbuf(stdout, NULL, _IOLBF, 0);
    machine_install_signals();
    machine_init(&m, M_SEPARATION, cfg, telem_fd);
    printf("%s[separation] started (pid %d)\n", log_stamp(), (int)m.pid);

    n = machine_run_stage(&m, in_fd, NULL, sep_done, &ctx);

    printf("%s[separation] finished: %u batches | paste %.2f kg = oil %.2f + "
           "water %.2f + pomace %.2f kg | earlier losses %.2f kg\n",
           log_stamp(), n, ctx.input, ctx.oil, ctx.water, ctx.pomace,
           ctx.losses);
    close_fd(&in_fd);
    close_fd(&result_fd);       /* -> EOF for the Supervisor's result reader */
    close_fd(&m.telem_fd);
    return 0;
}
