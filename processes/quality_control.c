/*
 * quality_control.c - Quality Control process (owner: Ibrahim).
 *
 * An independent process: it never touches a batch. It only reads the
 * telemetry that all six machines write on one shared pipe, compares every
 * reading with the per-machine limits from config.txt and tells the
 * Supervisor when a fault starts or ends.
 *
 *   6 machines --[MachineEvent]--> QC --[Alarm]--> Supervisor
 *                                    \--[MachineEvent copy]--> Supervisor (optional)
 *
 * Rules, per machine (see docs/qc_alarm_table.md):
 *   OVERHEAT      raise  temperature > MAX_TEMPERATURE
 *                 clear  temperature < RECOVERY_TEMPERATURE
 *   OVERPRESSURE  raise  pressure    > MAX_PRESSURE
 *                 clear  pressure    < RECOVERY_PRESSURE
 *   TIMEOUT       raise  batch_time  > MAX_BATCH_TIME (paused time excluded)
 *                 clear  machine moves to another batch / no batch
 *   MACHINE_ERROR raise  state == ERROR (automatic or injected failure)
 *                 clear  state != ERROR (Supervisor sent SIG_RESUME)
 *   A machine's EV_SHUTDOWN clears all of its active alarms.
 *
 * No alarm spam: QC keeps an "active" flag per (machine, fault) and only
 * sends a message on a transition. The gap between the max and the recovery
 * threshold (hysteresis) stops an alarm from flapping when a noisy reading
 * hovers around the limit.
 */
#define _POSIX_C_SOURCE 200809L
#include "processes.h"
#include "simulation.h"
#include "ipc.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void qc_init(QcState *qc)
{
    memset(qc, 0, sizeof *qc);
    for (int m = 0; m < NUM_MACHINES; m++)
        qc->timeout_batch[m] = -1;
}

static int push(QcState *qc, Alarm *out, int n, const MachineEvent *ev,
                int fault, int kind, double measured, double limit)
{
    Alarm *a = &out[n];

    a->machine  = ev->machine;
    a->batch_id = ev->batch_id;
    a->fault    = fault;
    a->kind     = kind;
    a->measured = measured;
    a->limit    = limit;
    qc->active[ev->machine][fault] = (kind == ALARM_RAISED);
    if (kind == ALARM_RAISED)
        qc->raised_count++;
    else
        qc->cleared_count++;
    return n + 1;
}

int qc_evaluate(QcState *qc, const Config *cfg, const MachineEvent *ev,
                Alarm out[NUM_FAULTS])
{
    const MachineConfig *mc;
    const unsigned char *act;
    int m = ev->machine, n = 0;
    int down = (ev->event == EV_SHUTDOWN);

    if (m < 0 || m >= NUM_MACHINES)
        return 0;
    qc->events_seen++;
    mc  = &cfg->machine[m];
    act = qc->active[m];

    /* temperature */
    if (!act[FAULT_OVERHEAT]) {
        if (!down && ev->temperature > mc->max_temperature)
            n = push(qc, out, n, ev, FAULT_OVERHEAT, ALARM_RAISED,
                     ev->temperature, mc->max_temperature);
    } else if (down || ev->temperature < mc->recovery_temperature) {
        n = push(qc, out, n, ev, FAULT_OVERHEAT, ALARM_CLEARED,
                 ev->temperature, mc->recovery_temperature);
    }

    /* pressure */
    if (!act[FAULT_OVERPRESSURE]) {
        if (!down && ev->pressure > mc->max_pressure)
            n = push(qc, out, n, ev, FAULT_OVERPRESSURE, ALARM_RAISED,
                     ev->pressure, mc->max_pressure);
    } else if (down || ev->pressure < mc->recovery_pressure) {
        n = push(qc, out, n, ev, FAULT_OVERPRESSURE, ALARM_CLEARED,
                 ev->pressure, mc->recovery_pressure);
    }

    /* processing time of one batch (0 disables the check) */
    if (mc->max_batch_time > 0.0) {
        if (!act[FAULT_TIMEOUT]) {
            if (!down && ev->batch_id >= 0 &&
                ev->batch_time > mc->max_batch_time) {
                qc->timeout_batch[m] = ev->batch_id;
                n = push(qc, out, n, ev, FAULT_TIMEOUT, ALARM_RAISED,
                         ev->batch_time, mc->max_batch_time);
            }
        } else if (down || ev->batch_id != qc->timeout_batch[m]) {
            qc->timeout_batch[m] = -1;
            n = push(qc, out, n, ev, FAULT_TIMEOUT, ALARM_CLEARED,
                     ev->batch_time, mc->max_batch_time);
        }
    }

    /* machine failure */
    if (!act[FAULT_MACHINE_ERROR]) {
        if (!down && ev->state == ST_ERROR)
            n = push(qc, out, n, ev, FAULT_MACHINE_ERROR, ALARM_RAISED,
                     ST_ERROR, 0);
    } else if (down || ev->state != ST_ERROR) {
        n = push(qc, out, n, ev, FAULT_MACHINE_ERROR, ALARM_CLEARED,
                 ev->state, 0);
    }
    return n;
}

static volatile sig_atomic_t qc_stop;

static void qc_on_stop(int sig)
{
    (void)sig;
    qc_stop = 1;
}

static void qc_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = qc_on_stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    /* QC is not a machine: pause/resume/injections do not apply to it. */
    signal(SIG_PAUSE, SIG_IGN);
    signal(SIG_RESUME, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
}

static void log_alarm(const Alarm *a)
{
    const char *unit = a->fault == FAULT_OVERHEAT ? "C"
                     : a->fault == FAULT_OVERPRESSURE ? "bar"
                     : a->fault == FAULT_TIMEOUT ? "s" : "";

    if (a->fault == FAULT_MACHINE_ERROR)
        printf("%s[qc] %s %s: %s (batch %d)\n", log_stamp(),
               a->kind == ALARM_RAISED ? "ALARM   " : "RECOVERY",
               machine_name(a->machine), fault_name(a->fault), a->batch_id);
    else
        printf("%s[qc] %s %s: %s %.2f%s %s %.2f%s (batch %d)\n", log_stamp(),
               a->kind == ALARM_RAISED ? "ALARM   " : "RECOVERY",
               machine_name(a->machine), fault_name(a->fault),
               a->measured, unit, a->kind == ALARM_RAISED ? ">" : "<",
               a->limit, unit, a->batch_id);
}

int quality_control_process(const Config *cfg, int telem_fd, int alarm_fd,
                            int forward_fd)
{
    QcState qc;
    MachineEvent ev;
    Alarm alarms[NUM_FAULTS];
    int stopped_by_signal = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    qc_stop = 0;
    qc_signals();
    qc_init(&qc);
    printf("%s[qc] started (pid %d)\n", log_stamp(), (int)getpid());

    for (;;) {
        struct pollfd p = { .fd = telem_fd, .events = POLLIN };
        ssize_t r;
        int k;

        if (qc_stop) {
            stopped_by_signal = 1;
            break;
        }
        k = poll(&p, 1, 200);       /* wake up regularly to see SIGTERM */
        if (k < 0 && errno != EINTR) {
            perror("qc: poll");
            break;
        }
        if (k <= 0)
            continue;

        r = read_full(telem_fd, &ev, sizeof ev);
        if (r == 0)
            break;                  /* EOF: every machine closed its end */
        if (r < 0) {
            perror("qc: read telemetry");
            break;
        }

        if (forward_fd >= 0 && write_full(forward_fd, &ev, sizeof ev) < 0) {
            perror("qc: forward status");
            close_fd(&forward_fd);
        }

        k = qc_evaluate(&qc, cfg, &ev, alarms);
        for (int i = 0; i < k; i++) {
            log_alarm(&alarms[i]);
            if (alarm_fd >= 0 &&
                write_full(alarm_fd, &alarms[i], sizeof alarms[i]) < 0) {
                perror("qc: write alarm");
                close_fd(&alarm_fd);
            }
        }
    }

    printf("%s[qc] finished (%s): %lu readings, %lu alarms raised, "
           "%lu recovered\n", log_stamp(),
           stopped_by_signal ? "SIGTERM" : "all machines done",
           qc.events_seen, qc.raised_count, qc.cleared_count);
    close_fd(&telem_fd);
    close_fd(&alarm_fd);
    close_fd(&forward_fd);
    return 0;
}
