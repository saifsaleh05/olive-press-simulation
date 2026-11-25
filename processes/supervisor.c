#define _POSIX_C_SOURCE 200809L
#include "supervisor.h"

#include "ipc.h"
#include "processes.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STOP_GRACE_SEC 2.0
#define MAX_MSGS_PER_POLL 512
#define MAX_KEEP_FDS 8

enum { PIPE_EVENT, PIPE_ALARM, PIPE_RESULT, NUM_UPLINKS };

static struct {
    const Config *cfg;
    FactorySnapshot s;
    struct timespec t0;
    int uplink[NUM_UPLINKS];
    int operator_paused;
    double stop_deadline;
    int killed;
} sv = { .uplink = { -1, -1, -1 } };

static volatile sig_atomic_t stop_flag;
static volatile sig_atomic_t stop_signalled;

static const char *process_name(int id)
{
    return id == M_QC ? "quality-control" : machine_name(id);
}

void supervisor_request_stop(void)
{
    stop_flag = 1;
    stop_signalled = 1;
}

int supervisor_stop_signalled(void)
{
    return stop_signalled;
}

static double uptime(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - sv.t0.tv_sec) +
           (double)(now.tv_nsec - sv.t0.tv_nsec) / 1e9;
}

void supervisor_log(const char *fmt, ...)
{
    char *line = sv.s.log[sv.s.log_total % LOG_LINES];
    int prefix;
    va_list ap;

    prefix = snprintf(line, LOG_WIDTH, "[%6.2f] ", uptime());
    va_start(ap, fmt);
    vsnprintf(line + prefix, LOG_WIDTH - (size_t)prefix, fmt, ap);
    va_end(ap);
    sv.s.log_total++;
    printf("%s\n", line);
    fflush(stdout);
}

static int is_machine(int id)
{
    return id >= 0 && id < NUM_MACHINES;
}

static void signal_process(int id, int signal_number)
{
    MachineView *view;

    if (id < 0 || id >= NUM_PROCS)
        return;
    view = &sv.s.m[id];
    if (view->alive && view->pid > 0 &&
        kill(view->pid, signal_number) < 0 && errno != ESRCH) {
        supervisor_log("signal %d to %s pid %d failed: %s",
                       signal_number, process_name(id), (int)view->pid,
                       strerror(errno));
    }
}

int supervisor_init(const Config *cfg)
{
    struct sigaction action;

    memset(&sv.s, 0, sizeof sv.s);
    sv.cfg = cfg;
    sv.operator_paused = 0;
    sv.killed = 0;
    stop_flag = 0;
    stop_signalled = 0;
    clock_gettime(CLOCK_MONOTONIC, &sv.t0);
    for (int i = 0; i < NUM_PROCS; ++i) {
        sv.s.m[i].batch_id = -1;
        sv.s.m[i].temperature = cfg->ambient_temperature;
        sv.s.m[i].state = ST_IDLE;
    }
    sv.s.line = LINE_READY;

    memset(&action, 0, sizeof action);
    action.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &action, NULL);
    return 0;
}

static void close_pair(int pipefd[2])
{
    if (pipefd[0] >= 0)
        close(pipefd[0]);
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    pipefd[0] = pipefd[1] = -1;
}

static void close_all_except(const int *keep, int keep_count)
{
    long maximum = sysconf(_SC_OPEN_MAX);

    if (maximum < 0 || maximum > 4096)
        maximum = 4096;
    for (int fd = 3; fd < maximum; ++fd) {
        int index = 0;
        while (index < keep_count && keep[index] != fd)
            index++;
        if (index == keep_count)
            close(fd);
    }
}

static int run_child(int id, const Config *cfg,
                     int batch[NUM_BATCH_PIPES][2], int telemetry[2],
                     int alarm[2], int result[2], int status[2])
{
    int keep[MAX_KEEP_FDS];
    int keep_count = 0;

    if (id == M_QC) {
        keep[keep_count++] = telemetry[0];
        keep[keep_count++] = alarm[1];
        keep[keep_count++] = status[1];
        close_all_except(keep, keep_count);
        return quality_control_process(cfg, telemetry[0], alarm[1], status[1]);
    }

    keep[keep_count++] = telemetry[1];
    if (id > M_RECEPTION)
        keep[keep_count++] = batch[id - 1][0];
    if (id < M_SEPARATION)
        keep[keep_count++] = batch[id][1];
    if (id == M_SEPARATION)
        keep[keep_count++] = result[1];
    close_all_except(keep, keep_count);

    switch (id) {
    case M_RECEPTION:
        return reception_process(cfg, batch[0][1], telemetry[1]);
    case M_CLEANING:
        return cleaning_process(cfg, batch[0][0], batch[1][1], telemetry[1]);
    case M_WASHING:
        return washing_process(cfg, batch[1][0], batch[2][1], telemetry[1]);
    case M_CRUSHING:
        return crushing_process(cfg, batch[2][0], batch[3][1], telemetry[1]);
    case M_MALAXATION:
        return malaxation_process(cfg, batch[3][0], batch[4][1], telemetry[1]);
    case M_SEPARATION:
        return separation_process(cfg, batch[4][0], result[1], telemetry[1]);
    default:
        return 1;
    }
}

int supervisor_start(void)
{
    int batch[NUM_BATCH_PIPES][2];
    int telemetry[2] = { -1, -1 };
    int alarm[2] = { -1, -1 };
    int result[2] = { -1, -1 };
    int status[2] = { -1, -1 };
    int started = 0;

    if (sv.s.line != LINE_READY)
        return -1;
    for (int i = 0; i < NUM_BATCH_PIPES; ++i)
        batch[i][0] = batch[i][1] = -1;
    for (int i = 0; i < NUM_BATCH_PIPES; ++i) {
        if (pipe(batch[i]) < 0)
            goto pipe_failure;
    }
    if (pipe(telemetry) < 0 || pipe(alarm) < 0 ||
        pipe(result) < 0 || pipe(status) < 0)
        goto pipe_failure;

    fflush(NULL);
    for (int id = 0; id < NUM_PROCS; ++id) {
        pid_t pid = fork();
        if (pid < 0) {
            supervisor_log("fork for %s failed: %s", process_name(id),
                           strerror(errno));
            for (int previous = 0; previous < started; ++previous)
                signal_process(previous, SIGTERM);
            goto pipe_failure;
        }
        if (pid == 0)
            _exit(run_child(id, sv.cfg, batch, telemetry, alarm, result, status));

        sv.s.m[id].pid = pid;
        sv.s.m[id].alive = 1;
        if (id == M_QC)
            sv.s.m[id].state = ST_OPERATING;
        started++;
    }

    for (int i = 0; i < NUM_BATCH_PIPES; ++i)
        close_pair(batch[i]);
    close_pair(telemetry);
    close(alarm[1]); alarm[1] = -1;
    close(result[1]); result[1] = -1;
    close(status[1]); status[1] = -1;

    sv.uplink[PIPE_EVENT] = status[0];
    sv.uplink[PIPE_ALARM] = alarm[0];
    sv.uplink[PIPE_RESULT] = result[0];
    sv.s.line = LINE_RUNNING;
    supervisor_log("line started: %.1f kg olives, seed %u",
                   sv.cfg->total_olives_kg, sv.cfg->seed);
    for (int id = 0; id < NUM_PROCS; ++id)
        supervisor_log("  %-15s pid %d", process_name(id),
                       (int)sv.s.m[id].pid);
    return 0;

pipe_failure:
    for (int i = 0; i < NUM_BATCH_PIPES; ++i)
        close_pair(batch[i]);
    close_pair(telemetry);
    close_pair(alarm);
    close_pair(result);
    close_pair(status);
    for (int i = 0; i < started; ++i)
        signal_process(i, SIGTERM);
    while (waitpid(-1, NULL, 0) > 0)
        ;
    return -1;
}

static int line_active(void)
{
    return sv.s.line == LINE_RUNNING || sv.s.line == LINE_PAUSED;
}

void supervisor_pause(void)
{
    if (sv.s.line != LINE_RUNNING)
        return;
    for (int id = 0; id < NUM_MACHINES; ++id)
        signal_process(id, SIG_PAUSE);
    sv.operator_paused = 1;
    sv.s.line = LINE_PAUSED;
    supervisor_log("operator: pause");
}

void supervisor_resume(void)
{
    if (sv.s.line != LINE_PAUSED)
        return;
    for (int id = 0; id < NUM_MACHINES; ++id) {
        if (id != M_RECEPTION || sv.s.faulted_mask == 0)
            signal_process(id, SIG_RESUME);
    }
    sv.operator_paused = 0;
    sv.s.line = LINE_RUNNING;
    supervisor_log("operator: resume");
}

static void hold_reception(void)
{
    if (!sv.operator_paused)
        signal_process(M_RECEPTION, SIG_PAUSE);
}

static void release_reception(void)
{
    if (!sv.operator_paused && sv.s.faulted_mask == 0)
        signal_process(M_RECEPTION, SIG_RESUME);
}

static void raise_fault(int id, int fault, double measured, double limit)
{
    MachineView *view;

    if (!is_machine(id) || !sv.s.m[id].alive ||
        (sv.s.faulted_mask & (1 << id)))
        return;
    view = &sv.s.m[id];
    view->fault = fault;
    view->fault_value = measured;
    view->fault_limit = limit;
    sv.s.faulted_mask |= 1 << id;
    if (fault == FAULT_MANUAL || fault == FAULT_MACHINE_ERROR)
        signal_process(id, SIG_FAULT);
    else
        signal_process(id, SIG_PAUSE);
    if (id != M_RECEPTION)
        hold_reception();
}

void supervisor_inject_fault(int id)
{
    if (!line_active() || !is_machine(id))
        return;
    supervisor_log("operator: manual fault on %s", machine_name(id));
    raise_fault(id, FAULT_MANUAL, 0.0, 0.0);
}

void supervisor_clear_fault(int id)
{
    if (!is_machine(id) || !(sv.s.faulted_mask & (1 << id)))
        return;
    sv.s.faulted_mask &= ~(1 << id);
    signal_process(id, SIG_RESUME);
    supervisor_log("%s fault cleared (%s)", machine_name(id),
                   fault_name(sv.s.m[id].fault));
    release_reception();
}

void supervisor_stop(void)
{
    if (sv.s.line == LINE_READY) {
        sv.s.line = LINE_FINISHED;
        return;
    }
    if (sv.s.line == LINE_STOPPING || sv.s.line == LINE_FINISHED)
        return;
    supervisor_log("stopping line");
    for (int id = 0; id < NUM_PROCS; ++id)
        signal_process(id, SIGTERM);
    sv.stop_deadline = uptime() + STOP_GRACE_SEC;
    sv.s.line = LINE_STOPPING;
}

static void on_event(const MachineEvent *event)
{
    MachineView *view;
    int previous;

    if (!is_machine(event->machine))
        return;
    view = &sv.s.m[event->machine];
    previous = view->state;
    view->state = event->state;
    view->batch_id = event->batch_id;
    view->temperature = event->temperature;
    view->pressure = event->pressure;
    view->active_time = event->active_time;
    view->progress = event->progress;
    view->last_seen = uptime();
    if (event->event == EV_BATCH_START)
        view->batches_in++;
    else if (event->event == EV_BATCH_DONE)
        view->batches_out++;

    if (event->event == EV_BATCH_START && event->machine == M_RECEPTION)
        supervisor_log("batch %d received", event->batch_id);
    if (previous != event->state &&
        (event->state == ST_ERROR || event->state == ST_COMPLETED ||
         previous == ST_ERROR)) {
        supervisor_log("%s: %s -> %s", machine_name(event->machine),
                       state_name(previous), state_name(event->state));
    }
}

static void on_alarm(const Alarm *alarm)
{
    if (!is_machine(alarm->machine))
        return;
    if (alarm->kind == ALARM_CLEARED) {
        if (sv.s.faulted_mask & (1 << alarm->machine))
            supervisor_clear_fault(alarm->machine);
        return;
    }

    sv.s.alarms++;
    supervisor_log("QC ALARM %s batch %d: %s %.2f > %.2f",
                   machine_name(alarm->machine), alarm->batch_id,
                   fault_name(alarm->fault), alarm->measured, alarm->limit);
    if (line_active())
        raise_fault(alarm->machine, alarm->fault,
                    alarm->measured, alarm->limit);
}

static void on_result(const ProductionResult *production)
{
    double output = production->oil + production->water + production->pomace +
                    production->losses;
    double error = fabs(production->original_mass - output);

    sv.s.produced++;
    sv.s.mass_in += production->original_mass;
    sv.s.oil += production->oil;
    sv.s.water += production->water;
    sv.s.pomace += production->pomace;
    sv.s.losses += production->losses;
    if (error > sv.s.balance_error)
        sv.s.balance_error = error;
    supervisor_log("batch %d done: %.1f kg -> oil %.1f, water %.1f, "
                   "pomace %.1f, losses %.1f", production->batch_id,
                   production->original_mass, production->oil,
                   production->water, production->pomace, production->losses);
    if (error > 1e-6)
        supervisor_log("WARNING batch %d mass balance error %.6f kg",
                       production->batch_id, error);
}

static int read_uplink(int which)
{
    union { MachineEvent event; Alarm alarm; ProductionResult result; } message;
    size_t size = which == PIPE_EVENT ? sizeof message.event
                : which == PIPE_ALARM ? sizeof message.alarm
                : sizeof message.result;
    ssize_t received = read_full(sv.uplink[which], &message, size);

    if (received <= 0) {
        if (received < 0)
            supervisor_log("uplink %d failed: %s", which, strerror(errno));
        close_fd(&sv.uplink[which]);
        return received < 0 ? -1 : 0;
    }
    if (which == PIPE_EVENT)
        on_event(&message.event);
    else if (which == PIPE_ALARM)
        on_alarm(&message.alarm);
    else
        on_result(&message.result);
    return 1;
}

static void reap_children(void)
{
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int id = 0;
        while (id < NUM_PROCS && sv.s.m[id].pid != pid)
            id++;
        if (id == NUM_PROCS)
            continue;
        sv.s.m[id].alive = 0;
        sv.s.m[id].exit_status = status;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            sv.s.m[id].state = ST_COMPLETED;
            supervisor_log("%s pid %d exited", process_name(id), (int)pid);
        } else {
            sv.s.child_failures++;
            sv.s.m[id].state = ST_ERROR;
            sv.s.m[id].fault = FAULT_CHILD_DIED;
            if (WIFSIGNALED(status))
                supervisor_log("%s pid %d killed by signal %d",
                               process_name(id), (int)pid, WTERMSIG(status));
            else
                supervisor_log("%s pid %d exited with status %d",
                               process_name(id), (int)pid,
                               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            if (line_active())
                supervisor_stop();
        }
    }
}

static void update_line_state(void)
{
    int alive = 0;
    for (int id = 0; id < NUM_PROCS; ++id)
        alive |= sv.s.m[id].alive;

    if (sv.s.line == LINE_STOPPING && alive && !sv.killed &&
        uptime() > sv.stop_deadline) {
        supervisor_log("shutdown grace expired; sending SIGKILL");
        for (int id = 0; id < NUM_PROCS; ++id)
            signal_process(id, SIGKILL);
        sv.killed = 1;
    }
    if (sv.s.line != LINE_READY && sv.s.line != LINE_FINISHED && !alive) {
        for (int i = 0; i < NUM_UPLINKS; ++i)
            while (sv.uplink[i] >= 0 && read_uplink(i) > 0)
                ;
        sv.s.line = LINE_FINISHED;
        supervisor_log("line finished: %d/%d batches, %.1f kg oil from %.1f kg",
                       sv.s.produced, sv.s.m[M_RECEPTION].batches_out,
                       sv.s.oil, sv.s.mass_in);
    }
}

int supervisor_poll(int timeout_ms)
{
    struct pollfd descriptors[NUM_UPLINKS];
    int handled = 0;

    if (stop_flag) {
        stop_flag = 0;
        supervisor_stop();
    }
    while (handled < MAX_MSGS_PER_POLL) {
        int map[NUM_UPLINKS];
        int count = 0;
        int ready;

        for (int i = 0; i < NUM_UPLINKS; ++i) {
            if (sv.uplink[i] < 0)
                continue;
            descriptors[count].fd = sv.uplink[i];
            descriptors[count].events = POLLIN;
            descriptors[count].revents = 0;
            map[count++] = i;
        }
        if (count == 0)
            break;
        ready = poll(descriptors, (nfds_t)count, handled ? 0 : timeout_ms);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            supervisor_log("poll failed: %s", strerror(errno));
            break;
        }
        if (ready == 0)
            break;
        for (int i = 0; i < count; ++i) {
            if (descriptors[i].revents & (POLLIN | POLLHUP | POLLERR))
                handled += read_uplink(map[i]) > 0;
        }
    }
    if (sv.s.line != LINE_READY) {
        reap_children();
        update_line_state();
    }
    return handled;
}

void supervisor_shutdown(void)
{
    if (sv.s.line == LINE_RUNNING || sv.s.line == LINE_PAUSED)
        supervisor_stop();
    while (sv.s.line == LINE_STOPPING)
        supervisor_poll(50);
    for (int i = 0; i < NUM_UPLINKS; ++i)
        close_fd(&sv.uplink[i]);
}

int supervisor_finished(void)
{
    return sv.s.line == LINE_FINISHED;
}

void supervisor_snapshot(FactorySnapshot *out)
{
    sv.s.uptime = uptime();
    *out = sv.s;
}
