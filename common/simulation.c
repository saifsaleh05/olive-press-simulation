/* simulation.c - shared machine model and stage run-time. See simulation.h */
#define _POSIX_C_SOURCE 200809L
#include "simulation.h"
#include "ipc.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Control flags, written by the signal handlers (and by the automatic fault
 * logic of the main loop). "Last signal wins": pause/resume/fault set the
 * wanted condition, the main loop derives the machine state from them on its
 * next tick. Nothing else happens inside a handler. */
static volatile sig_atomic_t g_paused;
static volatile sig_atomic_t g_error;
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_heat_req;
static volatile sig_atomic_t g_pressure_req;

static void on_pause(int sig)    { (void)sig; g_paused = 1; }
static void on_resume(int sig)   { (void)sig; g_paused = 0; g_error = 0; }
static void on_fault(int sig)    { (void)sig; g_error = 1; }
static void on_heat(int sig)     { (void)sig; g_heat_req = 1; }
static void on_pressure(int sig) { (void)sig; g_pressure_req = 1; }
static void on_stop(int sig)     { (void)sig; g_stop = 1; }

static void set_handler(int sig, void (*fn)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(sig, &sa, NULL);
}

void machine_install_signals(void)
{
    g_paused = g_error = g_stop = g_heat_req = g_pressure_req = 0;
    set_handler(SIG_PAUSE, on_pause);
    set_handler(SIG_RESUME, on_resume);
    set_handler(SIG_FAULT, on_fault);
    set_handler(SIG_HEAT, on_heat);
    set_handler(SIG_PRESSURE, on_pressure);
    set_handler(SIGTERM, on_stop);
    set_handler(SIGINT, on_stop);
    signal(SIGPIPE, SIG_IGN);       /* broken pipe -> write() returns EPIPE */
}

int machine_stop_requested(void)
{
    return g_stop != 0;
}

double now_seconds(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

const char *log_stamp(void)
{
    static char buf[32];
    struct timeval tv;
    struct tm tm;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    snprintf(buf, sizeof buf, "[%02d:%02d:%02d.%03ld] ", tm.tm_hour,
             tm.tm_min, tm.tm_sec, (long)(tv.tv_usec / 1000));
    return buf;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);           /* a signal may cut it short: fine */
}

static unsigned int mix32(unsigned int x)
{
    x += 0x9E3779B9u;
    x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
    x = (x ^ (x >> 13)) * 0xC2B2AE35u;
    return x ^ (x >> 16);
}

double simulation_random_unit(unsigned int seed, unsigned int stream,
                              unsigned int sample)
{
    unsigned int x = mix32(seed ^ mix32(stream) ^ mix32(sample));

    return (double)x / 4294967296.0;
}

double machine_random(Machine *m)
{
    /* xorshift32: a small, fully specified generator for repeatable readings. */
    unsigned int x = m->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->rng = x;
    return (double)x / 4294967296.0;
}

double machine_processing_time(const Config *cfg, int machine,
                               double batch_mass)
{
    double load;

    if (!cfg || machine < 0 || machine >= NUM_MACHINES ||
        cfg->nominal_batch_kg <= 0.0)
        return 0.0;
    load = batch_mass / cfg->nominal_batch_kg;
    if (load < 0.1)
        load = 0.1;
    return cfg->machine[machine].base_time * load;
}

void machine_init(Machine *m, int id, const Config *cfg, int telem_fd)
{
    memset(m, 0, sizeof *m);
    m->id          = id;
    m->pid         = getpid();
    m->cfg         = cfg;
    m->mc          = &cfg->machine[id];
    m->telem_fd    = telem_fd;
    m->state       = ST_IDLE;
    m->temperature = cfg->ambient_temperature;
    m->pressure    = 0.0;
    m->batch_id    = -1;
    m->fail_at     = -1.0;
    /* Own deterministic stream per process; never allow xorshift's zero state. */
    m->rng         = mix32(cfg->seed ^ (unsigned int)(id + 1));
    if (m->rng == 0)
        m->rng = 0xA341316Cu;
}

void machine_emit(Machine *m, int event)
{
    MachineEvent ev;
    double tn = m->cfg->temperature_noise, pn = m->cfg->pressure_noise;

    if (m->telem_fd < 0)
        return;
    memset(&ev, 0, sizeof ev);
    ev.machine      = m->id;
    ev.pid          = m->pid;
    ev.batch_id     = m->batch_id;
    ev.state        = m->state;
    ev.temperature  = m->temperature + (machine_random(m) * 2.0 - 1.0) * tn;
    ev.pressure     = m->pressure;
    if (m->pressure > 0.0) {
        ev.pressure += (machine_random(m) * 2.0 - 1.0) * pn;
        if (ev.pressure < 0.0)
            ev.pressure = 0.0;
    }
    ev.active_time  = m->active_time;
    ev.batch_time   = m->batch_time;
    ev.progress     = m->progress;
    ev.event        = event;
    ev.batches_done = m->batches_done;

    if (write_full(m->telem_fd, &ev, sizeof ev) < 0) {
        fprintf(stderr, "%s[%s] telemetry pipe closed (%s), continuing "
                "without telemetry\n", log_stamp(), machine_name(m->id),
                strerror(errno));
        close_fd(&m->telem_fd);
    }
}

void machine_physics_step(Machine *m, double dt)
{
    const MachineConfig *mc = m->mc;
    int    running = (m->state == ST_OPERATING);
    double H, target, a;

    if (dt <= 0.0)
        return;
    if (dt > 1.0)                    /* keep the explicit Euler step stable */
        dt = 1.0;

    /* T += (H - C (T - T_ambient)) dt */
    H = running ? mc->heating_rate : 0.0;
    m->temperature += (H - mc->cooling_factor *
                       (m->temperature - m->cfg->ambient_temperature)) * dt;

    /* pressure follows the load with a first-order lag */
    target = running ? mc->base_pressure + mc->pressure_gain * m->load : 0.0;
    a = dt / m->cfg->pressure_time_constant;
    if (a > 1.0)
        a = 1.0;
    m->pressure += (target - m->pressure) * a;
    if (m->pressure < 0.0)
        m->pressure = 0.0;
}

static void start_batch(Machine *m, const Batch *b)
{
    const Config *cfg = m->cfg;

    m->batch_id    = b->id;
    m->load        = b->current_mass / cfg->nominal_batch_kg;
    m->needed_time = machine_processing_time(cfg, m->id, b->current_mass);
    m->batch_time  = 0.0;
    m->progress    = 0.0;
    m->fail_at     = -1.0;
    if (cfg->auto_faults_enabled &&
        machine_random(m) < cfg->fault_probability)
        m->fail_at = 0.2 + 0.6 * machine_random(m);   /* somewhere mid-batch */
}

static void end_batch(Machine *m)
{
    m->batch_id    = -1;
    m->load        = 0.0;
    m->needed_time = 0.0;
    m->batch_time  = 0.0;
    m->progress    = 0.0;
    m->fail_at     = -1.0;
}

/* Apply one-shot manual injections (heat / pressure spikes). */
static void apply_injections(Machine *m)
{
    if (g_heat_req) {
        g_heat_req = 0;
        m->temperature += m->cfg->heat_spike;
        printf("%s[%s] manual fault: temperature +%.1fC -> %.1fC\n",
               log_stamp(), machine_name(m->id), m->cfg->heat_spike,
               m->temperature);
    }
    if (g_pressure_req) {
        g_pressure_req = 0;
        m->pressure += m->cfg->pressure_spike;
        printf("%s[%s] manual fault: pressure +%.1fbar -> %.2fbar\n",
               log_stamp(), machine_name(m->id), m->cfg->pressure_spike,
               m->pressure);
    }
}

unsigned int machine_run_stage(Machine *m, int in_fd,
                               stage_tick_fn tick, stage_done_fn done,
                               void *ctx)
{
    Batch  b;
    int    have = 0, upstream_eof = 0, broken = 0;
    int    tick_ms = m->cfg->tick_ms;
    double last = now_seconds();

    memset(&b, 0, sizeof b);
    machine_emit(m, EV_STATE_CHANGE);

    while (!g_stop) {
        double t = now_seconds(), dt = t - last;
        MachineState ns;
        int changed;

        last = t;
        apply_injections(m);

        /* 1. control flags -> actual state */
        if (g_error)
            ns = ST_ERROR;
        else if (g_paused)
            ns = ST_PAUSED;
        else
            ns = have ? ST_OPERATING : ST_IDLE;
        changed = (ns != m->state);
        if (changed && ns != ST_OPERATING && ns != ST_IDLE)
            printf("%s[%s] %s%s\n", log_stamp(), machine_name(m->id),
                   state_name(ns), have ? " (batch kept)" : "");
        m->state = ns;

        /* 2. model, 3. progress (only while OPERATING) */
        machine_physics_step(m, dt);
        if (have && m->state == ST_OPERATING) {
            m->active_time += dt;
            m->batch_time  += dt;
            m->progress = m->batch_time / m->needed_time;
            if (m->progress > 1.0)
                m->progress = 1.0;
            if (tick)
                tick(m, &b, dt, ctx);
            if (m->fail_at >= 0.0 && m->progress >= m->fail_at) {
                m->fail_at = -1.0;
                g_error = 1;        /* automatic failure, state on next tick */
                printf("%s[%s] automatic failure during batch %d\n",
                       log_stamp(), machine_name(m->id), b.id);
            }
        }

        /* 4. telemetry */
        machine_emit(m, changed ? EV_STATE_CHANGE : EV_HEARTBEAT);

        /* 5. finished batch -> hand it over */
        if (have && m->progress >= 1.0 && m->state == ST_OPERATING) {
            b.stage = m->id;
            if (done(m, &b, ctx) != 0) {
                broken = 1;
                break;
            }
            m->batches_done++;
            machine_emit(m, EV_BATCH_DONE);
            have = 0;
            end_batch(m);
            continue;
        }

        /* 6. wait: for the next batch when idle, otherwise one tick */
        if (!have && upstream_eof)
            break;
        if (!have && m->state == ST_IDLE) {
            struct pollfd p = { .fd = in_fd, .events = POLLIN };

            if (poll(&p, 1, tick_ms) > 0) {
                ssize_t n = read_full(in_fd, &b, sizeof b);

                if (n == (ssize_t)sizeof b) {
                    have = 1;
                    start_batch(m, &b);
                    m->state = ST_OPERATING;
                    machine_emit(m, EV_BATCH_START);
                } else {
                    if (n < 0)
                        perror(machine_name(m->id));
                    upstream_eof = 1;   /* every writer closed its end */
                }
            }
        } else {
            sleep_ms(tick_ms);
        }
    }

    if (have)
        fprintf(stderr, "%s[%s] stopped with batch %d in progress (%.0f%%)\n",
                log_stamp(), machine_name(m->id), b.id, m->progress * 100.0);

    m->state = (upstream_eof && !broken) ? ST_COMPLETED : ST_IDLE;
    m->batch_id = -1;
    m->batch_time = 0.0;
    m->progress = 0.0;
    machine_emit(m, EV_STATE_CHANGE);
    machine_emit(m, EV_SHUTDOWN);
    return m->batches_done;
}

int machine_run_source_batch(Machine *m, Batch *b,
                             stage_tick_fn tick, stage_done_fn done,
                             void *ctx)
{
    int tick_ms = m->cfg->tick_ms;
    double last = now_seconds();

    start_batch(m, b);
    m->state = ST_OPERATING;
    machine_emit(m, EV_BATCH_START);

    while (!g_stop) {
        double t = now_seconds(), dt = t - last;
        MachineState next_state;
        int changed;

        last = t;
        apply_injections(m);
        if (g_error)
            next_state = ST_ERROR;
        else if (g_paused)
            next_state = ST_PAUSED;
        else
            next_state = ST_OPERATING;
        changed = next_state != m->state;
        m->state = next_state;

        machine_physics_step(m, dt);
        if (m->state == ST_OPERATING) {
            m->active_time += dt;
            m->batch_time += dt;
            m->progress = m->batch_time / m->needed_time;
            if (m->progress > 1.0)
                m->progress = 1.0;
            if (tick)
                tick(m, b, dt, ctx);
            if (m->fail_at >= 0.0 && m->progress >= m->fail_at) {
                m->fail_at = -1.0;
                g_error = 1;
            }
        }
        machine_emit(m, changed ? EV_STATE_CHANGE : EV_HEARTBEAT);

        if (m->progress >= 1.0 && m->state == ST_OPERATING) {
            b->stage = m->id;
            if (done && done(m, b, ctx) != 0)
                return -1;
            m->batches_done++;
            machine_emit(m, EV_BATCH_DONE);
            end_batch(m);
            m->state = ST_IDLE;
            machine_emit(m, EV_STATE_CHANGE);
            return 0;
        }
        sleep_ms(tick_ms);
    }
    return -1;
}
