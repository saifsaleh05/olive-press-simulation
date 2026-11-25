/*
 * Stand-in implementations of the seven child processes.
 *
 * They follow the real protocol (batch pipes, MachineEvent telemetry,
 * Alarm, ProductionResult, control signals) so the supervisor and GUI can
 * be built and tested before the real stages are merged. Every function is
 * weak: as soon as processes/reception.c (or any other stage) defines
 * reception_run(), the linker picks that one and this mock is ignored.
 */
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "config.h"
#include "ipc.h"

#define WEAK __attribute__((weak))

static volatile sig_atomic_t paused, faulted, terminate;

static void on_signal(int sig)
{
    if (sig == SIG_PAUSE)        paused = 1;
    else if (sig == SIG_RESUME)  paused = 0;
    else if (sig == SIG_FAULT)   faulted = 1;
    else if (sig == SIG_CLEAR)   faulted = 0;
    else if (sig == SIGTERM)     terminate = 1;
}

static void install_handlers(void)
{
    struct sigaction sa;
    const int sigs[] = { SIG_PAUSE, SIG_RESUME, SIGTERM };

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* no SA_RESTART: a signal should wake poll()/nanosleep() right away */
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
    sigaction(SIG_FAULT, &sa, NULL);
    sigaction(SIG_CLEAR, &sa, NULL);
}

/* xorshift32, seeded per stage so every run with the same seed matches */
static double rnd01(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x ? x : 0x9e3779b9u;
    return (double)(*s >> 8) / 16777216.0;
}

static double rnd_range(uint32_t *s, Range r)
{
    return r.min + (r.max - r.min) * rnd01(s);
}

static uint32_t mix_seed(unsigned seed, int a, int b)
{
    uint32_t h = seed * 2654435761u ^ (uint32_t)(a + 1) * 40503u ^ (uint32_t)(b + 7) * 2246822519u;
    return h ? h : 1u;
}

static double now_sec(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

typedef struct {
    const StageCtx   *ctx;
    const MachineCfg *mc;
    uint32_t rng;
    int      state;
    int      batch_id;
    double   temp;
    double   pressure;
    double   active;
    double   progress;
    double   last_t;
} Machine;

static int effective_state(int working)
{
    if (faulted) return ST_ERROR;
    if (paused)  return ST_PAUSED;
    return working ? ST_OPERATING : ST_IDLE;
}

static void emit(Machine *m, int event)
{
    MachineEvent ev = {
        .machine = m->ctx->id, .pid = getpid(), .batch_id = m->batch_id,
        .state = m->state, .temperature = m->temp, .pressure = m->pressure,
        .active_time = m->active, .progress = m->progress, .event = event,
    };

    if (write_full(m->ctx->telem_fd, &ev, sizeof ev) < 0)
        terminate = 1;      /* supervisor is gone, nothing left to report to */
}

/* Advance the thermal model by the real time since the previous call. */
static void step(Machine *m, int working, double load)
{
    const Config *c = m->ctx->cfg;
    double t = now_sec(), dt = t - m->last_t;
    int    st = effective_state(working);
    double heat = st == ST_OPERATING ? m->mc->heat_rate * load : 0.0;
    double p_target = st == ST_OPERATING ? m->mc->base_pressure : 1.0;

    m->last_t = t;
    m->temp += (heat - m->mc->cool_rate * (m->temp - c->ambient_temp)) * dt;
    m->pressure += (p_target - m->pressure) * fmin(1.0, 3.0 * dt);
    m->pressure += (rnd01(&m->rng) - 0.5) * 0.04 * m->mc->base_pressure;
    if (st == ST_OPERATING)
        m->active += dt;

    if (st != m->state) {
        m->state = st;
        emit(m, EV_STATE_CHANGE);
    } else {
        emit(m, EV_TELEMETRY);
    }
}

static void nap(double sec)
{
    struct timespec ts = { (time_t)sec, (long)((sec - (double)(time_t)sec) * 1e9) };
    nanosleep(&ts, NULL);   /* EINTR is fine, the caller re-checks the flags */
}

static void machine_init(Machine *m, const StageCtx *ctx)
{
    memset(m, 0, sizeof *m);
    m->ctx = ctx;
    m->mc = &ctx->cfg->m[ctx->id];
    m->rng = mix_seed(ctx->cfg->seed, ctx->id, -1);
    m->state = ST_IDLE;
    m->batch_id = -1;
    m->temp = ctx->cfg->ambient_temp;
    m->pressure = 1.0;
    m->last_t = now_sec();
    install_handlers();
    emit(m, EV_STATE_CHANGE);
}

/* Spend one batch worth of OPERATING time; pauses and faults stretch it. */
static int process_batch(Machine *m, const Batch *b)
{
    const Config *c = m->ctx->cfg;
    double duration = rnd_range(&m->rng, m->mc->duration);
    double avg_mass = 0.5 * (c->batch_mass.min + c->batch_mass.max);
    double load = b->current_mass / avg_mass;
    double done = 0.0;

    m->batch_id = b->id;
    m->progress = 0.0;
    emit(m, EV_BATCH_IN);

    while (done < duration) {
        double before = m->active;

        nap(c->tick);
        if (terminate)
            return -1;
        step(m, 1, load);
        done += m->active - before;
        m->progress = fmin(1.0, done / duration);
    }
    return 0;
}

static void finish(Machine *m)
{
    m->batch_id = -1;
    m->progress = terminate ? 0.0 : 1.0;
    /* blue only means the work really ran out, not that we were told to stop */
    m->state = terminate ? ST_IDLE : ST_COMPLETED;
    emit(m, EV_STATE_CHANGE);
    emit(m, EV_EXIT);
}

static void apply_stage(int id, const Config *c, Batch *b, double temp)
{
    double removal = c->m[id].debris_removal;

    if (removal > 0.0) {
        double debris = b->current_mass * b->impurity * removal;
        double left = b->current_mass * b->impurity - debris;

        b->current_mass -= debris;
        b->impurity = b->current_mass > 0.0 ? left / b->current_mass : 0.0;
    }
    /* paste quality suffers above the cold extraction limit */
    if (temp > 27.0 && (id == M_CRUSHING || id == M_MALAXATION))
        b->quality -= (temp - 27.0) * 1.5;
    b->stage = id;
}

static int run_reception(const StageCtx *ctx)
{
    const Config *c = ctx->cfg;
    Machine m;

    machine_init(&m, ctx);
    for (int i = 0; i < c->num_batches && !terminate; i++) {
        uint32_t r = mix_seed(c->seed, 100, i);
        Batch b = { .id = i + 1, .quality = 100.0, .stage = M_RECEPTION };

        /* hold the container while paused or faulted */
        while ((paused || faulted) && !terminate) {
            step(&m, 0, 0.0);
            nap(c->tick);
        }

        b.original_mass = rnd_range(&r, c->batch_mass);
        b.current_mass  = b.original_mass;
        b.impurity      = rnd_range(&r, c->impurity);
        b.oil_potential = rnd_range(&r, c->oil_potential);

        if (process_batch(&m, &b) < 0)
            break;
        if (write_full(ctx->out_fd, &b, sizeof b) < 0)
            break;
        emit(&m, EV_BATCH_OUT);
        m.batch_id = -1;
        m.progress = 0.0;
    }

    close(ctx->out_fd);     /* EOF tells the rest of the line we are done */
    finish(&m);
    return 0;
}

static int run_stage(const StageCtx *ctx)
{
    const Config *c = ctx->cfg;
    Machine m;

    machine_init(&m, ctx);
    while (!terminate) {
        struct pollfd pfd = { .fd = ctx->in_fd, .events = POLLIN };
        Batch  b;
        ssize_t n;
        int    rc;

        if (paused || faulted) {
            step(&m, 0, 0.0);
            nap(c->tick);
            continue;
        }

        rc = poll(&pfd, 1, (int)(c->tick * 1000));
        if (rc <= 0) {
            step(&m, 0, 0.0);   /* idle: cool down and keep reporting */
            continue;
        }

        n = read_full(ctx->in_fd, &b, sizeof b);
        if (n <= 0)
            break;              /* upstream closed its end: no more batches */

        if (process_batch(&m, &b) < 0)
            break;
        apply_stage(ctx->id, c, &b, m.temp);

        if (ctx->id == M_SEPARATION) {
            uint32_t r = mix_seed(c->seed, 200, b.id);
            double   wf = rnd_range(&r, c->water_fraction);
            ProductionResult pr = { .batch_id = b.id, .original_mass = b.original_mass };

            pr.oil    = b.current_mass * b.oil_potential * c->extraction_eff;
            pr.water  = b.current_mass * wf;
            pr.pomace = b.current_mass - pr.oil - pr.water;
            pr.losses = b.original_mass - b.current_mass;
            if (write_full(ctx->result_fd, &pr, sizeof pr) < 0)
                break;
        } else if (write_full(ctx->out_fd, &b, sizeof b) < 0) {
            break;
        }

        emit(&m, EV_BATCH_OUT);
        m.batch_id = -1;
        m.progress = 0.0;
        step(&m, 0, 0.0);
    }

    if (ctx->out_fd >= 0)
        close(ctx->out_fd);
    finish(&m);
    return 0;
}

static int run_qc(const StageCtx *ctx)
{
    const Config *c = ctx->cfg;
    int temp_armed[NUM_MACHINES], pres_armed[NUM_MACHINES];
    MachineEvent self = { .machine = M_QC, .pid = getpid(), .batch_id = -1,
                          .state = ST_OPERATING, .temperature = c->ambient_temp,
                          .pressure = 1.0, .event = EV_STATE_CHANGE };
    MachineEvent ev;

    install_handlers();
    for (int i = 0; i < NUM_MACHINES; i++)
        temp_armed[i] = pres_armed[i] = 1;
    write_full(ctx->telem_fd, &self, sizeof self);

    while (!terminate) {
        ssize_t n = read_full(ctx->qc_in_fd, &ev, sizeof ev);
        const MachineCfg *mc;

        if (n <= 0)
            break;
        if (ev.machine < 0 || ev.machine >= NUM_MACHINES)
            continue;
        mc = &c->m[ev.machine];

        if (temp_armed[ev.machine] && ev.temperature > mc->max_temp) {
            Alarm a = { ev.machine, ev.batch_id, FAULT_OVERTEMP, ev.temperature, mc->max_temp };
            write_full(ctx->alarm_fd, &a, sizeof a);
            temp_armed[ev.machine] = 0;
        } else if (!temp_armed[ev.machine] && ev.state != ST_ERROR &&
                   ev.temperature < mc->max_temp) {
            temp_armed[ev.machine] = 1;
        }

        if (pres_armed[ev.machine] && ev.pressure > mc->max_pressure) {
            Alarm a = { ev.machine, ev.batch_id, FAULT_OVERPRESSURE, ev.pressure, mc->max_pressure };
            write_full(ctx->alarm_fd, &a, sizeof a);
            pres_armed[ev.machine] = 0;
        } else if (!pres_armed[ev.machine] && ev.state != ST_ERROR &&
                   ev.pressure < mc->max_pressure) {
            pres_armed[ev.machine] = 1;
        }
    }

    self.state = ST_COMPLETED;
    self.event = EV_EXIT;
    write_full(ctx->telem_fd, &self, sizeof self);
    return 0;
}

WEAK int reception_run(const StageCtx *ctx)        { return run_reception(ctx); }
WEAK int cleaning_run(const StageCtx *ctx)         { return run_stage(ctx); }
WEAK int washing_run(const StageCtx *ctx)          { return run_stage(ctx); }
WEAK int crushing_run(const StageCtx *ctx)         { return run_stage(ctx); }
WEAK int malaxation_run(const StageCtx *ctx)       { return run_stage(ctx); }
WEAK int separation_run(const StageCtx *ctx)       { return run_stage(ctx); }
WEAK int quality_control_run(const StageCtx *ctx)  { return run_qc(ctx); }
