#ifndef OLIVE_SUPERVISOR_H
#define OLIVE_SUPERVISOR_H

#include <sys/types.h>

#include "common.h"
#include "config.h"

#define LOG_LINES 14
#define LOG_WIDTH 100

typedef enum {
    LINE_READY = 0,     /* nothing forked yet */
    LINE_RUNNING,
    LINE_PAUSED,
    LINE_STOPPING,      /* SIGTERM sent, waiting for children */
    LINE_FINISHED       /* every child reaped */
} LineState;

typedef struct {
    int    state;           /* last state the child itself reported */
    pid_t  pid;
    int    alive;
    int    exit_status;     /* raw waitpid() status once reaped */
    int    batch_id;
    double temperature;
    double pressure;
    double active_time;
    double progress;
    int    batches_in;
    int    batches_out;
    int    fault;           /* FaultCode of the active / last fault */
    double fault_value;
    double fault_limit;
    double last_seen;       /* supervisor uptime of the last event */
} MachineView;

typedef struct {
    LineState   line;
    double      uptime;
    MachineView m[NUM_PROCS];

    int    produced;        /* ProductionResult messages received */
    double mass_in;
    double oil;
    double water;
    double pomace;
    double losses;
    double balance_error;   /* worst |in - out| seen on one batch, kg */

    int    alarms;
    int    faulted_mask;    /* bit i set while machine i is held in ERROR */
    int    qc_dropped;      /* telemetry copies QC could not keep up with */

    char   log[LOG_LINES][LOG_WIDTH];
    int    log_total;       /* lines ever written, newest is (total-1) % LOG_LINES */
} FactorySnapshot;

int  supervisor_init(const Config *cfg);
int  supervisor_start(void);

/*
 * Drain whatever is waiting on the telemetry, alarm and result pipes, reap
 * children and run fault handling. Waits at most timeout_ms for the first
 * message; 0 makes it a pure non blocking check (what the GUI uses).
 */
int  supervisor_poll(int timeout_ms);

void supervisor_pause(void);
void supervisor_resume(void);
void supervisor_inject_fault(int machine);
void supervisor_clear_fault(int machine);
void supervisor_stop(void);
void supervisor_shutdown(void);

int  supervisor_finished(void);
void supervisor_snapshot(FactorySnapshot *out);
void supervisor_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Async-signal-safe: lets SIGINT/SIGTERM in the parent request a stop. */
void supervisor_request_stop(void);
int  supervisor_stop_signalled(void);

#endif
