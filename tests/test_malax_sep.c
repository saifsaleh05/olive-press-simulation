/*
 * test_malax_sep.c - console test: Malaxation -> Separation (+ QC).
 *
 * Forks the real malaxation_process, separation_process and
 * quality_control_process, connected with pipes exactly like in the full
 * program. The parent plays Crushing (feeds batches with varied properties)
 * and a minimal Supervisor:
 *   - pauses Malaxation for a while, then resumes it        (operator)
 *   - injects a failure into Separation, repairs it later   (manual fault)
 *   - injects a heat spike into Malaxation                  (manual fault)
 *   - on an OVERHEAT/OVERPRESSURE alarm pauses that machine, and resumes it
 *     when QC reports the recovery (automatic recovery)
 * It then checks: every batch arrives once and in order, mass is conserved,
 * a paused batch keeps its progress, the alarms were raised and recovered,
 * EOF propagates and every child exits with status 0.
 *
 * Usage: ./build/test_malax_sep [config.txt]
 */
#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "ipc.h"
#include "processes.h"
#include "simulation.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

#define NBATCH 6

static const Batch INPUT[NBATCH] = {
 /* id  original  current impurity oil   quality stage            */
    { 1, 25.0, 24.10, 0.03, 0.18, 0.90, M_CRUSHING },  /* typical         */
    { 2, 20.0, 19.50, 0.01, 0.15, 0.60, M_CRUSHING },  /* light, poor     */
    { 3, 30.0, 28.60, 0.05, 0.25, 1.00, M_CRUSHING },  /* heavy, rich     */
    { 4, 22.0, 21.00, 0.02, 0.21, 0.75, M_CRUSHING },
    { 5, 28.0, 26.00, 0.05, 0.17, 0.85, M_CRUSHING },  /* large losses    */
    { 6, 26.0, 25.40, 0.02, 0.23, 0.95, M_CRUSHING },
};

static pid_t spawn(const char *name, int (*fn)(void *), void *arg)
{
    pid_t pid;

    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        perror(name);
        exit(2);
    }
    if (pid == 0)
        _exit(fn(arg));
    return pid;
}

/* fds are set up in main() before fork, each child closes what it does not
 * use (the EOF rule) and runs its process function. */
static int feed[2], m2s[2], result[2], telem[2], alarm_p[2], status_p[2];
static Config cfg;

static int run_qc(void *arg)
{
    (void)arg;
    close(feed[0]); close(feed[1]); close(m2s[0]); close(m2s[1]);
    close(result[0]); close(result[1]); close(telem[1]);
    close(alarm_p[0]); close(status_p[0]);
    return quality_control_process(&cfg, telem[0], alarm_p[1], status_p[1]);
}

static int run_malax(void *arg)
{
    (void)arg;
    close(feed[1]); close(m2s[0]); close(result[0]); close(result[1]);
    close(telem[0]); close(alarm_p[0]); close(alarm_p[1]);
    close(status_p[0]); close(status_p[1]);
    return malaxation_process(&cfg, feed[0], m2s[1], telem[1]);
}

static int run_sep(void *arg)
{
    (void)arg;
    close(feed[0]); close(feed[1]); close(m2s[1]); close(result[0]);
    close(telem[0]); close(alarm_p[0]); close(alarm_p[1]);
    close(status_p[0]); close(status_p[1]);
    return separation_process(&cfg, m2s[0], result[1], telem[1]);
}

int main(int argc, char **argv)
{
    char err[128];
    pid_t qc_pid, mx_pid, sp_pid, pid_of[NUM_MACHINES] = { 0 };
    ProductionResult res[NBATCH];
    int nres = 0, res_open = 1, alarm_open = 1, status_open = 1;
    int seen_raise[NUM_MACHINES][NUM_FAULTS] = { { 0 } };
    int seen_clear[NUM_MACHINES][NUM_FAULTS] = { { 0 } };
    int did_pause = 0, did_resume = 0, did_fault = 0, did_heat = 0;
    double repair_at = -1, t0;
    /* progress of Malaxation while operator-paused */
    int paused_batch = -2, paused_events = 0;
    double paused_min = 2, paused_max = -1;
    double in_total = 0, oil = 0, water = 0, pomace = 0, losses = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    config_defaults(&cfg);
    if (argc > 1 && config_load(&cfg, argv[1]) != 0)
        return 2;

    /* fast, deterministic test settings */
    cfg.tick_ms = 20;
    cfg.auto_faults_enabled = 0;
    cfg.heat_spike = 15.0;
    cfg.machine[M_MALAXATION].base_time = 0.8;
    cfg.machine[M_MALAXATION].cooling_factor = 0.5;   /* cools in ~2 s */
    cfg.machine[M_SEPARATION].base_time = 0.4;
    if (config_validate(&cfg, err, sizeof err) != 0) {
        fprintf(stderr, "invalid config: %s\n", err);
        return 2;
    }

    printf("== Malaxation -> Separation console test ==\n");
    if (pipe(feed) < 0 || pipe(m2s) < 0 || pipe(result) < 0 ||
        pipe(telem) < 0 || pipe(alarm_p) < 0 || pipe(status_p) < 0) {
        perror("pipe");
        return 2;
    }

    qc_pid = spawn("qc", run_qc, NULL);
    mx_pid = spawn("malaxation", run_malax, NULL);
    sp_pid = spawn("separation", run_sep, NULL);
    pid_of[M_MALAXATION] = mx_pid;
    pid_of[M_SEPARATION] = sp_pid;

    /* parent keeps: feed write end, result/alarm/status read ends */
    close(feed[0]); close(m2s[0]); close(m2s[1]); close(result[1]);
    close(telem[0]); close(telem[1]); close(alarm_p[1]); close(status_p[1]);

    for (int i = 0; i < NBATCH; i++) {
        printf("%s[test] crushing -> malaxation: batch %d (%.2f kg paste, "
               "oil %.0f%%, quality %.0f%%)\n", log_stamp(), INPUT[i].id,
               INPUT[i].current_mass, INPUT[i].oil_potential * 100,
               INPUT[i].quality * 100);
        write_full(feed[1], &INPUT[i], sizeof INPUT[i]);
    }
    close(feed[1]);                 /* no more batches -> EOF downstream */

    t0 = now_seconds();
    while (res_open || alarm_open || status_open) {
        struct pollfd p[3] = {
            { .fd = res_open ? result[0] : -1,   .events = POLLIN },
            { .fd = alarm_open ? alarm_p[0] : -1, .events = POLLIN },
            { .fd = status_open ? status_p[0] : -1, .events = POLLIN },
        };
        double t = now_seconds() - t0;

        if (t > 60) {
            CHECK(0, "test timed out");
            kill(mx_pid, SIGTERM); kill(sp_pid, SIGTERM); kill(qc_pid, SIGTERM);
            break;
        }

        /* scripted operator actions */
        if (!did_pause && t > 0.25) {
            did_pause = 1;
            printf("%s[test] operator: PAUSE malaxation\n", log_stamp());
            kill(mx_pid, SIG_PAUSE);
        }
        if (did_pause && !did_resume && t > 0.85) {
            did_resume = 1;
            printf("%s[test] operator: RESUME malaxation\n", log_stamp());
            kill(mx_pid, SIG_RESUME);
        }
        if (!did_fault && t > 1.2) {
            did_fault = 1;
            printf("%s[test] operator: inject FAILURE into separation\n",
                   log_stamp());
            kill(sp_pid, SIG_FAULT);
        }
        if (repair_at > 0 && t > repair_at) {
            repair_at = -1;
            printf("%s[test] operator: separation repaired -> RESUME\n",
                   log_stamp());
            kill(sp_pid, SIG_RESUME);
        }
        if (!did_heat && t > 2.0) {
            did_heat = 1;
            printf("%s[test] operator: inject HEAT spike into malaxation\n",
                   log_stamp());
            kill(mx_pid, SIG_HEAT);
        }

        if (poll(p, 3, 20) <= 0)
            continue;

        if (p[0].revents) {
            ProductionResult r;
            if (read_full(result[0], &r, sizeof r) == (ssize_t)sizeof r) {
                if (nres < NBATCH)
                    res[nres] = r;
                nres++;
            } else {
                res_open = 0;
            }
        }
        if (p[1].revents) {
            Alarm a;
            if (read_full(alarm_p[0], &a, sizeof a) == (ssize_t)sizeof a) {
                if (a.kind == ALARM_RAISED) {
                    seen_raise[a.machine][a.fault]++;
                    if (a.fault == FAULT_MACHINE_ERROR)
                        repair_at = now_seconds() - t0 + 0.4;
                    if ((a.fault == FAULT_OVERHEAT ||
                         a.fault == FAULT_OVERPRESSURE) && pid_of[a.machine]) {
                        printf("%s[test] supervisor: %s alarm -> PAUSE %s\n",
                               log_stamp(), fault_name(a.fault),
                               machine_name(a.machine));
                        kill(pid_of[a.machine], SIG_PAUSE);
                    }
                } else {
                    seen_clear[a.machine][a.fault]++;
                    if ((a.fault == FAULT_OVERHEAT ||
                         a.fault == FAULT_OVERPRESSURE) && pid_of[a.machine]) {
                        printf("%s[test] supervisor: %s recovered -> RESUME "
                               "%s\n", log_stamp(), fault_name(a.fault),
                               machine_name(a.machine));
                        kill(pid_of[a.machine], SIG_RESUME);
                    }
                }
            } else {
                alarm_open = 0;
            }
        }
        if (p[2].revents) {
            MachineEvent e;
            if (read_full(status_p[0], &e, sizeof e) == (ssize_t)sizeof e) {
                if (e.machine == M_MALAXATION && e.state == ST_PAUSED &&
                    did_pause && !did_resume && e.batch_id >= 0) {
                    if (paused_batch == -2)
                        paused_batch = e.batch_id;
                    CHECK(e.batch_id == paused_batch,
                          "batch changed while paused");
                    if (e.progress < paused_min) paused_min = e.progress;
                    if (e.progress > paused_max) paused_max = e.progress;
                    paused_events++;
                }
            } else {
                status_open = 0;
            }
        }
    }

    /* children */
    {
        pid_t pids[3] = { mx_pid, sp_pid, qc_pid };
        const char *names[3] = { "malaxation", "separation", "qc" };
        for (int i = 0; i < 3; i++) {
            int st;
            waitpid(pids[i], &st, 0);
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                  "%s exited abnormally", names[i]);
        }
    }

    printf("\nresults (Separation -> Supervisor)\n");
    printf("  %5s %8s %8s %7s | %7s %7s %7s | %8s %8s\n", "batch", "olives",
           "paste", "losses", "oil", "water", "pomace", "sum", "error");
    for (int i = 0; i < nres && i < NBATCH; i++) {
        const ProductionResult *r = &res[i];
        double sum = r->oil + r->water + r->pomace;
        double e = fabs(r->input_mass - sum);

        printf("  %5d %8.3f %8.3f %7.3f | %7.3f %7.3f %7.3f | %8.3f %8.1e\n",
               r->batch_id, r->original_mass, r->input_mass, r->losses,
               r->oil, r->water, r->pomace, sum, e);
        CHECK(r->batch_id == INPUT[i].id, "batch order: got %d expected %d",
              r->batch_id, INPUT[i].id);
        CHECK(fabs(r->input_mass - INPUT[i].current_mass) < 1e-12,
              "malaxation changed the paste mass of batch %d", r->batch_id);
        CHECK(e < 1e-9, "batch %d: paste != oil + water + pomace",
              r->batch_id);
        CHECK(fabs(r->original_mass - (r->losses + sum)) < 1e-9,
              "batch %d: original != losses + outputs", r->batch_id);
        in_total += r->original_mass;
        oil += r->oil; water += r->water; pomace += r->pomace;
        losses += r->losses;
    }
    printf("  total: olives %.3f = losses %.3f + oil %.3f + water %.3f + "
           "pomace %.3f (diff %.1e)\n", in_total, losses, oil, water, pomace,
           fabs(in_total - (losses + oil + water + pomace)));

    printf("\nchecks\n");
    printf("  batches received: %d/%d\n", nres, NBATCH);
    printf("  paused malaxation: batch %d, %d readings, progress %.3f..%.3f\n",
           paused_batch, paused_events, paused_min, paused_max);
    printf("  alarms: separation failure raised %d / cleared %d, "
           "malaxation overheat raised %d / cleared %d\n",
           seen_raise[M_SEPARATION][FAULT_MACHINE_ERROR],
           seen_clear[M_SEPARATION][FAULT_MACHINE_ERROR],
           seen_raise[M_MALAXATION][FAULT_OVERHEAT],
           seen_clear[M_MALAXATION][FAULT_OVERHEAT]);

    CHECK(nres == NBATCH, "expected %d results, got %d", NBATCH, nres);
    CHECK(paused_events > 5, "malaxation did not report PAUSED");
    CHECK(paused_events == 0 || paused_max - paused_min < 1e-12,
          "progress moved while paused");
    CHECK(seen_raise[M_SEPARATION][FAULT_MACHINE_ERROR] == 1 &&
          seen_clear[M_SEPARATION][FAULT_MACHINE_ERROR] == 1,
          "separation failure alarm/recovery");
    CHECK(seen_raise[M_MALAXATION][FAULT_OVERHEAT] == 1 &&
          seen_clear[M_MALAXATION][FAULT_OVERHEAT] == 1,
          "malaxation overheat alarm/recovery");

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
