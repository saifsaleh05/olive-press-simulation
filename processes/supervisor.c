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

#include "common.h"
#include "ipc.h"
#include "supervisor.h"

#define STOP_GRACE_SEC 2.0
#define MAX_MSGS_PER_POLL 512
#define MAX_KEEP_FDS 8

enum { PIPE_TELEM, PIPE_ALARM, PIPE_RESULT, NUM_UPLINKS };

static stage_fn const stage_table[NUM_PROCS] = {
    reception_run, cleaning_run, washing_run,
    crushing_run, malaxation_run, separation_run,
    quality_control_run
};

static struct {
    const Config   *cfg;
    FactorySnapshot s;
    struct timespec t0;

    int    uplink[NUM_UPLINKS];   /* read ends, -1 once at EOF */
    int    qc_feed;               /* write end, nonblocking */

    int    operator_paused;       /* pause came from the operator, not a fault */
    double stop_deadline;
    int    killed;
} sv = {
    .uplink  = { -1, -1, -1 },
    .qc_feed = -1,
};

static volatile sig_atomic_t stop_flag, stop_signalled;

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
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - sv.t0.tv_sec) + (double)(t.tv_nsec - sv.t0.tv_nsec) / 1e9;
}

void supervisor_log(const char *fmt, ...)
{
    char   *line = sv.s.log[sv.s.log_total % LOG_LINES];
    double  t = uptime();
    int     n;
    va_list ap;

    n = snprintf(line, LOG_WIDTH, "[%6.2f] ", t);
    va_start(ap, fmt);
    vsnprintf(line + n, LOG_WIDTH - (size_t)n, fmt, ap);
    va_end(ap);
    sv.s.log_total++;

    printf("%s\n", line);
    fflush(stdout);
}

static int is_stage(int id)
{
    return id >= 0 && id < NUM_MACHINES;
}

static void signal_machine(int id, int sig)
{
    MachineView *m = &sv.s.m[id];

    if (m->alive && m->pid > 0 && kill(m->pid, sig) < 0 && errno != ESRCH)
        supervisor_log("kill(%d, %d) for %s failed: %s", (int)m->pid, sig,
                       machine_name(id), strerror(errno));
}

int supervisor_init(const Config *cfg)
{
    struct sigaction sa;

    memset(&sv.s, 0, sizeof sv.s);
    sv.cfg = cfg;
    clock_gettime(CLOCK_MONOTONIC, &sv.t0);

    for (int i = 0; i < NUM_PROCS; i++) {
        sv.s.m[i].batch_id = -1;
        sv.s.m[i].temperature = cfg->ambient_temp;
        sv.s.m[i].state = ST_IDLE;
    }
    sv.s.line = LINE_READY;

    /* A dead QC must not take the parent down with it on the next forward. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    return 0;
}

/* ---------------------------------------------------------------- startup */

static void close_all_except(const int *keep, int nkeep)
{
    long max = sysconf(_SC_OPEN_MAX);

    if (max < 0 || max > 4096)
        max = 4096;

    /*
     * Brute force on purpose: besides our own pipes this also closes
     * anything the parent inherited or opened (the X11 socket when the
     * GUI is running), so a child can never hold the display or keep a
     * pipe from reaching EOF.
     */
    for (int fd = 3; fd < max; fd++) {
        int k = 0;
        while (k < nkeep && keep[k] != fd)
            k++;
        if (k == nkeep)
            close(fd);
    }
}

static void child_signal_defaults(void)
{
    struct sigaction sa;
    sigset_t         none;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa, NULL);   /* Ctrl-C is the supervisor's business */
    sigaction(SIGPIPE, &sa, NULL);  /* report EPIPE instead of dying */

    sa.sa_handler = SIG_DFL;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);

    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

static void close_pair(int p[2])
{
    if (p[0] >= 0) close(p[0]);
    if (p[1] >= 0) close(p[1]);
    p[0] = p[1] = -1;
}

int supervisor_start(void)
{
    int batch[NUM_BATCH_PIPES][2];
    int telem[2], alarm[2], result[2], qcfeed[2];
    int ok = 1;

    if (sv.s.line != LINE_READY)
        return -1;

    for (int i = 0; i < NUM_BATCH_PIPES; i++)
        batch[i][0] = batch[i][1] = -1;
    telem[0] = telem[1] = alarm[0] = alarm[1] = -1;
    result[0] = result[1] = qcfeed[0] = qcfeed[1] = -1;

    for (int i = 0; i < NUM_BATCH_PIPES && ok; i++)
        ok = pipe(batch[i]) == 0;
    ok = ok && pipe(telem) == 0 && pipe(alarm) == 0 && pipe(result) == 0 && pipe(qcfeed) == 0;
    if (!ok) {
        supervisor_log("pipe() failed: %s", strerror(errno));
        goto fail_pipes;
    }

    /* Anything still sitting in stdio buffers would be printed once per child. */
    fflush(NULL);

    for (int id = 0; id < NUM_PROCS; id++) {
        pid_t pid = fork();

        if (pid < 0) {
            supervisor_log("fork() for %s failed: %s", machine_name(id), strerror(errno));
            for (int k = 0; k < id; k++)
                signal_machine(k, SIGKILL);
            goto fail_pipes;
        }

        if (pid == 0) {
            StageCtx ctx = {
                .id = id, .cfg = sv.cfg,
                .in_fd = -1, .out_fd = -1, .telem_fd = telem[1],
                .result_fd = -1, .alarm_fd = -1, .qc_in_fd = -1,
            };
            int keep[MAX_KEEP_FDS], nkeep = 0;

            if (is_stage(id) && id > 0)
                ctx.in_fd = batch[id - 1][0];
            if (is_stage(id) && id < NUM_MACHINES - 1)
                ctx.out_fd = batch[id][1];
            if (id == M_SEPARATION)
                ctx.result_fd = result[1];
            if (id == M_QC) {
                ctx.alarm_fd = alarm[1];
                ctx.qc_in_fd = qcfeed[0];
            }

            keep[nkeep++] = ctx.telem_fd;
            if (ctx.in_fd >= 0)     keep[nkeep++] = ctx.in_fd;
            if (ctx.out_fd >= 0)    keep[nkeep++] = ctx.out_fd;
            if (ctx.result_fd >= 0) keep[nkeep++] = ctx.result_fd;
            if (ctx.alarm_fd >= 0)  keep[nkeep++] = ctx.alarm_fd;
            if (ctx.qc_in_fd >= 0)  keep[nkeep++] = ctx.qc_in_fd;

            close_all_except(keep, nkeep);
            child_signal_defaults();

            /* _exit: never run the parent's atexit handlers (freeglut has some). */
            _exit(stage_table[id](&ctx));
        }

        sv.s.m[id].pid = pid;
        sv.s.m[id].alive = 1;
    }

    /* Parent keeps only the read side of the uplinks and the write side of the QC feed. */
    for (int i = 0; i < NUM_BATCH_PIPES; i++)
        close_pair(batch[i]);
    close(telem[1]);
    close(alarm[1]);
    close(result[1]);
    close(qcfeed[0]);

    sv.uplink[PIPE_TELEM]  = telem[0];
    sv.uplink[PIPE_ALARM]  = alarm[0];
    sv.uplink[PIPE_RESULT] = result[0];
    sv.qc_feed = qcfeed[1];
    set_nonblocking(sv.qc_feed);

    sv.s.line = LINE_RUNNING;
    supervisor_log("line started, %d batches, seed %u", sv.cfg->num_batches, sv.cfg->seed);
    for (int id = 0; id < NUM_PROCS; id++)
        supervisor_log("  %-15s pid %d", machine_name(id), (int)sv.s.m[id].pid);
    return 0;

fail_pipes:
    for (int i = 0; i < NUM_BATCH_PIPES; i++)
        close_pair(batch[i]);
    close_pair(telem);
    close_pair(alarm);
    close_pair(result);
    close_pair(qcfeed);
    return -1;
}

/* --------------------------------------------------------------- controls */

static int line_active(void)
{
    return sv.s.line == LINE_RUNNING || sv.s.line == LINE_PAUSED;
}

void supervisor_pause(void)
{
    if (sv.s.line != LINE_RUNNING)
        return;
    for (int id = 0; id < NUM_MACHINES; id++)
        signal_machine(id, SIG_PAUSE);
    sv.operator_paused = 1;
    sv.s.line = LINE_PAUSED;
    supervisor_log("operator: pause");
}

void supervisor_resume(void)
{
    if (sv.s.line != LINE_PAUSED)
        return;
    for (int id = 0; id < NUM_MACHINES; id++) {
        /* Reception stays held while some machine is still in ERROR. */
        if (id == M_RECEPTION && sv.s.faulted_mask)
            continue;
        signal_machine(id, SIG_RESUME);
    }
    sv.operator_paused = 0;
    sv.s.line = LINE_RUNNING;
    supervisor_log("operator: resume");
}

static void hold_feed(void)
{
    if (!sv.operator_paused)
        signal_machine(M_RECEPTION, SIG_PAUSE);
}

static void release_feed(void)
{
    if (!sv.operator_paused && sv.s.faulted_mask == 0)
        signal_machine(M_RECEPTION, SIG_RESUME);
}

static void raise_fault(int id, int code, double value, double limit)
{
    MachineView *m = &sv.s.m[id];

    if (!is_stage(id) || !m->alive || (sv.s.faulted_mask & (1 << id)))
        return;

    m->fault = code;
    m->fault_value = value;
    m->fault_limit = limit;
    sv.s.faulted_mask |= 1 << id;

    signal_machine(id, SIG_FAULT);
    /* Stop feeding new olives while a machine is down; the rest drain. */
    if (id != M_RECEPTION)
        hold_feed();
}

void supervisor_inject_fault(int id)
{
    if (!line_active() || !is_stage(id))
        return;
    supervisor_log("operator: manual fault on %s", machine_name(id));
    raise_fault(id, FAULT_MANUAL, 0.0, 0.0);
}

void supervisor_clear_fault(int id)
{
    if (!is_stage(id) || !(sv.s.faulted_mask & (1 << id)))
        return;
    sv.s.faulted_mask &= ~(1 << id);
    signal_machine(id, SIG_CLEAR);
    supervisor_log("%s fault cleared (%s)", machine_name(id), fault_name(sv.s.m[id].fault));
    release_feed();
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
    for (int id = 0; id < NUM_PROCS; id++)
        signal_machine(id, SIGTERM);
    sv.stop_deadline = uptime() + STOP_GRACE_SEC;
    sv.s.line = LINE_STOPPING;
}

/* ---------------------------------------------------------- incoming data */

static void forward_to_qc(const MachineEvent *ev)
{
    if (sv.qc_feed < 0)
        return;
    if (write(sv.qc_feed, ev, sizeof *ev) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            sv.s.qc_dropped++;      /* QC is behind; it only needs fresh samples */
        else if (errno == EPIPE) {
            close(sv.qc_feed);
            sv.qc_feed = -1;
        }
    }
}

static void on_event(const MachineEvent *ev)
{
    MachineView *m;
    int          prev;

    if (ev->machine < 0 || ev->machine >= NUM_PROCS)
        return;

    m = &sv.s.m[ev->machine];
    prev = m->state;

    m->state       = ev->state;
    m->batch_id    = ev->batch_id;
    m->temperature = ev->temperature;
    m->pressure    = ev->pressure;
    m->active_time = ev->active_time;
    m->progress    = ev->progress;
    m->last_seen   = uptime();

    if (ev->event == EV_BATCH_IN)
        m->batches_in++;
    else if (ev->event == EV_BATCH_OUT)
        m->batches_out++;

    if (ev->event == EV_BATCH_IN && ev->machine == M_RECEPTION)
        supervisor_log("batch %d received", ev->batch_id);

    if (prev != ev->state && (ev->state == ST_ERROR || ev->state == ST_COMPLETED ||
                              prev == ST_ERROR))
        supervisor_log("%s: %s -> %s", machine_name(ev->machine),
                       state_name(prev), state_name(ev->state));

    if (ev->machine != M_QC)
        forward_to_qc(ev);
}

static void on_alarm(const Alarm *a)
{
    if (!is_stage(a->machine))
        return;

    sv.s.alarms++;
    supervisor_log("QC ALARM %s batch %d: %s %.2f > %.2f", machine_name(a->machine),
                   a->batch_id, fault_name(a->fault), a->value, a->limit);

    if (line_active())
        raise_fault(a->machine, a->fault, a->value, a->limit);
}

static void on_result(const ProductionResult *r)
{
    double out = r->oil + r->water + r->pomace + r->losses;
    double err = fabs(r->original_mass - out);

    sv.s.produced++;
    sv.s.mass_in += r->original_mass;
    sv.s.oil     += r->oil;
    sv.s.water   += r->water;
    sv.s.pomace  += r->pomace;
    sv.s.losses  += r->losses;
    if (err > sv.s.balance_error)
        sv.s.balance_error = err;

    supervisor_log("batch %d done: %.1f kg in -> oil %.1f, water %.1f, pomace %.1f, losses %.1f",
                   r->batch_id, r->original_mass, r->oil, r->water, r->pomace, r->losses);
    if (err > 1e-6)
        supervisor_log("WARNING batch %d mass balance off by %.6f kg", r->batch_id, err);
}

/* Returns 1 if a message was handled, 0 on EOF (fd closed), -1 on error. */
static int read_uplink(int which)
{
    union { MachineEvent ev; Alarm al; ProductionResult pr; } msg;
    size_t  len = which == PIPE_TELEM ? sizeof msg.ev
                : which == PIPE_ALARM ? sizeof msg.al : sizeof msg.pr;
    ssize_t n = read_full(sv.uplink[which], &msg, len);

    if (n <= 0) {
        if (n < 0)
            supervisor_log("uplink %d read error: %s", which, strerror(errno));
        close(sv.uplink[which]);
        sv.uplink[which] = -1;
        return n;
    }

    if (which == PIPE_TELEM)
        on_event(&msg.ev);
    else if (which == PIPE_ALARM)
        on_alarm(&msg.al);
    else
        on_result(&msg.pr);
    return 1;
}

static int stages_alive(void)
{
    int n = 0;

    for (int id = 0; id < NUM_MACHINES; id++)
        n += sv.s.m[id].alive;
    return n;
}

static void reap_children(void)
{
    pid_t pid;
    int   status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int id = 0;

        while (id < NUM_PROCS && sv.s.m[id].pid != pid)
            id++;
        if (id == NUM_PROCS)
            continue;

        MachineView *m = &sv.s.m[id];
        m->alive = 0;
        m->exit_status = status;

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            supervisor_log("%s (pid %d) exited", machine_name(id), (int)pid);
            continue;
        }

        if (WIFSIGNALED(status))
            supervisor_log("%s (pid %d) killed by signal %d", machine_name(id), (int)pid,
                           WTERMSIG(status));
        else
            supervisor_log("%s (pid %d) exited with status %d", machine_name(id), (int)pid,
                           WEXITSTATUS(status));

        if (line_active()) {
            m->state = ST_ERROR;
            m->fault = FAULT_CHILD_DIED;
            sv.s.alarms++;
            supervisor_stop();      /* the pipeline is broken, shut it down safely */
        }
    }

    /* Every stage drained and exited: QC has nothing left to watch. */
    if (sv.s.line != LINE_READY && stages_alive() == 0 && sv.qc_feed >= 0) {
        close(sv.qc_feed);
        sv.qc_feed = -1;
    }
}

static void auto_recover(void)
{
    if (!sv.cfg->auto_recover || sv.s.line == LINE_STOPPING)
        return;

    for (int id = 0; id < NUM_MACHINES; id++) {
        const MachineView *m = &sv.s.m[id];
        const MachineCfg  *c = &sv.cfg->m[id];
        double ratio = sv.cfg->recover_ratio;

        if (!(sv.s.faulted_mask & (1 << id)) || m->state != ST_ERROR)
            continue;
        if ((m->fault == FAULT_OVERTEMP && m->temperature < c->max_temp * ratio) ||
            (m->fault == FAULT_OVERPRESSURE && m->pressure < c->max_pressure * ratio))
            supervisor_clear_fault(id);
    }
}

static void update_line_state(void)
{
    int any_alive = 0;

    for (int id = 0; id < NUM_PROCS; id++)
        any_alive |= sv.s.m[id].alive;

    if (sv.s.line == LINE_STOPPING && any_alive && !sv.killed && uptime() > sv.stop_deadline) {
        supervisor_log("children did not exit in %.0fs, sending SIGKILL", STOP_GRACE_SEC);
        for (int id = 0; id < NUM_PROCS; id++)
            signal_machine(id, SIGKILL);
        sv.killed = 1;
    }

    if (sv.s.line != LINE_READY && sv.s.line != LINE_FINISHED && !any_alive) {
        /* Drain anything the children wrote right before exiting. */
        while (sv.uplink[PIPE_TELEM] >= 0 || sv.uplink[PIPE_ALARM] >= 0 ||
               sv.uplink[PIPE_RESULT] >= 0) {
            for (int i = 0; i < NUM_UPLINKS; i++)
                if (sv.uplink[i] >= 0)
                    read_uplink(i);
        }
        sv.s.line = LINE_FINISHED;
        supervisor_log("line finished: %d/%d batches, %.1f kg oil from %.1f kg olives",
                       sv.s.produced, sv.cfg->num_batches, sv.s.oil, sv.s.mass_in);
    }
}

int supervisor_poll(int timeout_ms)
{
    struct pollfd pfd[NUM_UPLINKS];
    int handled = 0;

    if (stop_flag) {
        stop_flag = 0;
        supervisor_stop();
    }

    while (handled < MAX_MSGS_PER_POLL) {
        int nfds = 0, map[NUM_UPLINKS], rc;

        for (int i = 0; i < NUM_UPLINKS; i++) {
            if (sv.uplink[i] < 0)
                continue;
            pfd[nfds].fd = sv.uplink[i];
            pfd[nfds].events = POLLIN;
            map[nfds++] = i;
        }
        if (nfds == 0)
            break;

        rc = poll(pfd, (nfds_t)nfds, handled ? 0 : timeout_ms);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            supervisor_log("poll: %s", strerror(errno));
            break;
        }
        if (rc == 0)
            break;

        for (int k = 0; k < nfds; k++)
            if (pfd[k].revents & (POLLIN | POLLHUP | POLLERR))
                handled += read_uplink(map[k]) > 0;
    }

    if (sv.s.line != LINE_READY) {
        reap_children();
        auto_recover();
        update_line_state();
    }
    return handled;
}

/* --------------------------------------------------------------- shutdown */

void supervisor_shutdown(void)
{
    if (sv.s.line == LINE_RUNNING || sv.s.line == LINE_PAUSED)
        supervisor_stop();

    while (sv.s.line == LINE_STOPPING)
        supervisor_poll(50);

    for (int i = 0; i < NUM_UPLINKS; i++) {
        if (sv.uplink[i] >= 0)
            close(sv.uplink[i]);
        sv.uplink[i] = -1;
    }
    if (sv.qc_feed >= 0)
        close(sv.qc_feed);
    sv.qc_feed = -1;
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
