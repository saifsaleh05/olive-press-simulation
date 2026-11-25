/*
 * simulation.h - shared machine model and run-time for the stage processes.
 *
 * A machine process owns at most one batch at a time and advances it in
 * ticks of TICK_MS. On every tick it:
 *   1. applies pending control signals (pause / resume / fault / stop),
 *   2. updates temperature and pressure (formulas below),
 *   3. advances progress only while OPERATING (never PAUSED or ERROR),
 *   4. sends a MachineEvent on the telemetry pipe.
 * The batch stays in the process memory while paused or failed, so a
 * pause/resume or failure/recovery never loses a batch.
 *
 * Temperature (team decision):
 *     T += (H - C * (T - T_ambient)) * dt      H = heating_rate while running,
 *                                              0 when idle / paused / failed
 * Pressure: follows base_pressure + pressure_gain * load while running
 * (load = batch kg / NOMINAL_BATCH_KG), 0 otherwise, with a first-order lag.
 * Processing time = base_time * load.
 * Sensor readings sent to QC add +-noise; the model itself has no noise.
 *
 * machine_run_stage() implements the loop once so every stage can reuse it;
 * the stage only supplies what happens to a batch while and after it is
 * processed.
 */
#ifndef SIMULATION_H
#define SIMULATION_H

#include <sys/types.h>
#include "common.h"
#include "config.h"

typedef struct {
    int                  id;           /* MachineId                       */
    pid_t                pid;
    const Config        *cfg;
    const MachineConfig *mc;
    int                  telem_fd;     /* -1 = no telemetry               */

    MachineState state;
    double       temperature;          /* model value, deg C              */
    double       pressure;             /* model value, bar                */
    double       active_time;          /* s operating since start-up      */
    double       batch_time;           /* s operating on current batch    */
    double       progress;             /* 0..1                            */
    double       needed_time;          /* s needed for the current batch  */
    double       load;                 /* batch kg / nominal kg           */
    double       fail_at;              /* progress at which an automatic
                                          failure happens, <0 = none      */
    int          batch_id;             /* -1 = none                       */
    unsigned int batches_done;
    unsigned int rng;                  /* rand_r() state, per process     */
} Machine;

/* Called on every OPERATING tick (may be NULL). */
typedef void (*stage_tick_fn)(Machine *m, Batch *b, double dt, void *ctx);

/* Called when a batch reaches progress 1.0: forward it / produce output.
 * Return 0 on success, -1 if the output channel is broken (stage stops). */
typedef int (*stage_done_fn)(Machine *m, Batch *b, void *ctx);

void   machine_init(Machine *m, int id, const Config *cfg, int telem_fd);

/* Install the SIG_* handlers, SIGTERM/SIGINT, ignore SIGPIPE.
 * Call once in the child after fork(). */
void   machine_install_signals(void);

/* Send the current status (with sensor noise) on the telemetry pipe. */
void   machine_emit(Machine *m, int event);

/* One model step of dt seconds; only ST_OPERATING heats / pressurises. */
void   machine_physics_step(Machine *m, double dt);

/* Uniform random number in [0,1) from the machine's own deterministic stream.
 * The implementation is project-owned, so results do not depend on libc's
 * rand()/rand_r() implementation. */
double machine_random(Machine *m);

/* Stateless deterministic sample. Separate stream numbers keep unrelated
 * consumers independent; sample numbers make a batch value independent of
 * scheduling and telemetry frequency. */
double simulation_random_unit(unsigned int seed, unsigned int stream,
                              unsigned int sample);

/* Weight-dependent duration shared by all stages. A tiny positive batch is
 * treated as 10% nominal load so processing never completes instantly. */
double machine_processing_time(const Config *cfg, int machine,
                               double batch_mass);

int    machine_stop_requested(void);

/* Generic stage loop: read Batch messages from in_fd until EOF (or SIGTERM),
 * process each one, call done() for it. Returns batches finished. */
unsigned int machine_run_stage(Machine *m, int in_fd,
                               stage_tick_fn tick, stage_done_fn done,
                               void *ctx);

/* Process one locally generated batch (Reception has no input pipe). */
int machine_run_source_batch(Machine *m, Batch *b,
                             stage_tick_fn tick, stage_done_fn done,
                             void *ctx);

/* Monotonic time in seconds. */
double now_seconds(void);

/* "[hh:mm:ss.mmm] " wall-clock prefix for log lines. */
const char *log_stamp(void);

#endif /* SIMULATION_H */
