/*
 * test_quality_control.c - Quality Control rules and process (owner: Ibrahim).
 *
 * Part 1 feeds scripted telemetry to qc_evaluate(): normal readings, each
 * fault scenario (overheat, overpressure, timeout, machine failure), repeated
 * violations (no duplicate alarms), the recovery thresholds (hysteresis) and
 * shutdown.
 * Part 2 forks the real quality_control_process(), talks to it only through
 * pipes like the Supervisor does, and checks the alarm stream, the status
 * forwarding and that QC exits by itself on EOF.
 *
 * Usage: ./build/test_quality_control [config.txt]
 */
#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "ipc.h"
#include "processes.h"

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
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                   \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

static Config cfg;
static QcState qc;

static MachineEvent ev(int machine, int batch, int state, double t,
                       double p, double batch_time, int event)
{
    MachineEvent e;

    memset(&e, 0, sizeof e);
    e.machine = machine;
    e.pid = 1000 + machine;
    e.batch_id = batch;
    e.state = state;
    e.temperature = t;
    e.pressure = p;
    e.batch_time = batch_time;
    e.event = event;
    return e;
}

/* Feed one reading, print it and the transitions it caused. */
static int feed(const char *what, MachineEvent e, Alarm out[NUM_FAULTS])
{
    int n = qc_evaluate(&qc, &cfg, &e, out);

    printf("  %-34s %-10s T=%5.1f P=%4.1f t=%5.1f -> ", what,
           machine_name(e.machine), e.temperature, e.pressure, e.batch_time);
    if (n == 0)
        printf("-");
    for (int i = 0; i < n; i++)
        printf("%s%s %s", i ? ", " : "",
               out[i].kind == ALARM_RAISED ? "RAISED" : "CLEARED",
               fault_name(out[i].fault));
    printf("\n");
    return n;
}

static int is(const Alarm *a, int machine, int fault, int kind)
{
    return a->machine == machine && a->fault == fault && a->kind == kind;
}

static void unit_tests(void)
{
    Alarm a[NUM_FAULTS];
    const MachineConfig *mx = &cfg.machine[M_MALAXATION];
    const MachineConfig *sp = &cfg.machine[M_SEPARATION];
    const MachineConfig *cr = &cfg.machine[M_CRUSHING];
    int n;

    qc_init(&qc);

    printf("1. normal readings\n");
    for (int m = 0; m < NUM_MACHINES; m++) {
        const MachineConfig *mc = &cfg.machine[m];
        n = feed("normal", ev(m, 1, ST_OPERATING, mc->recovery_temperature - 1,
                              mc->recovery_pressure - 0.5,
                              mc->max_batch_time / 2, EV_HEARTBEAT), a);
        CHECK(n == 0, "normal reading on %s raised an alarm", machine_name(m));
    }

    printf("2. overheat + recovery threshold (malaxation max %.1f, "
           "recovery %.1f)\n", mx->max_temperature, mx->recovery_temperature);
    n = feed("at the limit (not above)",
             ev(M_MALAXATION, 7, ST_OPERATING, mx->max_temperature, 1.5, 3,
                EV_HEARTBEAT), a);
    CHECK(n == 0, "value equal to the limit must not alarm");
    n = feed("above max",
             ev(M_MALAXATION, 7, ST_OPERATING, mx->max_temperature + 0.4, 1.5,
                3.1, EV_HEARTBEAT), a);
    CHECK(n == 1 && is(&a[0], M_MALAXATION, FAULT_OVERHEAT, ALARM_RAISED),
          "overheat not raised");
    CHECK(n == 1 && a[0].batch_id == 7 && a[0].limit == mx->max_temperature,
          "alarm carries batch and limit");
    n = feed("still hot (no duplicate)",
             ev(M_MALAXATION, 7, ST_PAUSED, mx->max_temperature + 1.0, 0, 3.1,
                EV_STATE_CHANGE), a);
    CHECK(n == 0, "duplicate overheat alarm");
    n = feed("below max, above recovery",
             ev(M_MALAXATION, 7, ST_PAUSED,
                (mx->max_temperature + mx->recovery_temperature) / 2, 0, 3.1,
                EV_HEARTBEAT), a);
    CHECK(n == 0, "cleared too early (hysteresis)");
    n = feed("back above max (still active)",
             ev(M_MALAXATION, 7, ST_PAUSED, mx->max_temperature + 0.2, 0, 3.1,
                EV_HEARTBEAT), a);
    CHECK(n == 0, "flapping alarm");
    n = feed("below recovery",
             ev(M_MALAXATION, 7, ST_PAUSED, mx->recovery_temperature - 0.1, 0,
                3.1, EV_HEARTBEAT), a);
    CHECK(n == 1 && is(&a[0], M_MALAXATION, FAULT_OVERHEAT, ALARM_CLEARED),
          "overheat not cleared");
    CHECK(n == 1 && a[0].limit == mx->recovery_temperature,
          "cleared alarm carries the recovery threshold");
    n = feed("cool and normal",
             ev(M_MALAXATION, 7, ST_OPERATING, mx->recovery_temperature - 1, 1.5,
                3.2, EV_HEARTBEAT), a);
    CHECK(n == 0, "duplicate recovery");

    printf("3. overpressure (separation max %.1f, recovery %.1f)\n",
           sp->max_pressure, sp->recovery_pressure);
    n = feed("pressure spike",
             ev(M_SEPARATION, 4, ST_OPERATING, 30, sp->max_pressure + 1, 1,
                EV_HEARTBEAT), a);
    CHECK(n == 1 && is(&a[0], M_SEPARATION, FAULT_OVERPRESSURE, ALARM_RAISED),
          "overpressure not raised");
    n = feed("between limits",
             ev(M_SEPARATION, 4, ST_OPERATING, 30,
                (sp->max_pressure + sp->recovery_pressure) / 2, 1.1,
                EV_HEARTBEAT), a);
    CHECK(n == 0, "overpressure cleared too early");
    n = feed("below recovery",
             ev(M_SEPARATION, 4, ST_OPERATING, 30, sp->recovery_pressure - 0.1,
                1.2, EV_HEARTBEAT), a);
    CHECK(n == 1 && is(&a[0], M_SEPARATION, FAULT_OVERPRESSURE, ALARM_CLEARED),
          "overpressure not cleared");

    printf("4. timeout (crushing max %.1f s per batch)\n", cr->max_batch_time);
    n = feed("slow batch",
             ev(M_CRUSHING, 3, ST_OPERATING, 28, 4, cr->max_batch_time + 0.5,
                EV_HEARTBEAT), a);
    CHECK(n == 1 && is(&a[0], M_CRUSHING, FAULT_TIMEOUT, ALARM_RAISED),
          "timeout not raised");
    n = feed("still slow (no duplicate)",
             ev(M_CRUSHING, 3, ST_OPERATING, 28, 4, cr->max_batch_time + 2,
                EV_HEARTBEAT), a);
    CHECK(n == 0, "duplicate timeout");
    n = feed("batch handed over",
             ev(M_CRUSHING, 3, ST_OPERATING, 28, 4, cr->max_batch_time + 2.1,
                EV_BATCH_DONE), a);
    CHECK(n == 0, "timeout cleared before the batch left");
    n = feed("next batch",
             ev(M_CRUSHING, 4, ST_OPERATING, 28, 4, 0.1, EV_BATCH_START), a);
    CHECK(n == 1 && is(&a[0], M_CRUSHING, FAULT_TIMEOUT, ALARM_CLEARED),
          "timeout not cleared on next batch");

    printf("5. manual / automatic machine failure\n");
    n = feed("failure injected",
             ev(M_WASHING, 9, ST_ERROR, 30, 0, 1, EV_STATE_CHANGE), a);
    CHECK(n == 1 && is(&a[0], M_WASHING, FAULT_MACHINE_ERROR, ALARM_RAISED),
          "machine error not raised");
    n = feed("still failed",
             ev(M_WASHING, 9, ST_ERROR, 29, 0, 1, EV_HEARTBEAT), a);
    CHECK(n == 0, "duplicate machine error");
    n = feed("resumed by supervisor",
             ev(M_WASHING, 9, ST_OPERATING, 29, 1.5, 1, EV_STATE_CHANGE), a);
    CHECK(n == 1 && is(&a[0], M_WASHING, FAULT_MACHINE_ERROR, ALARM_CLEARED),
          "machine error not cleared");

    printf("6. two faults in one reading, other machines unaffected\n");
    n = feed("hot and high pressure",
             ev(M_CRUSHING, 5, ST_OPERATING, cr->max_temperature + 2,
                cr->max_pressure + 1, 1, EV_HEARTBEAT), a);
    CHECK(n == 2 && is(&a[0], M_CRUSHING, FAULT_OVERHEAT, ALARM_RAISED) &&
          is(&a[1], M_CRUSHING, FAULT_OVERPRESSURE, ALARM_RAISED),
          "both faults expected");
    n = feed("malaxation normal meanwhile",
             ev(M_MALAXATION, 6, ST_OPERATING, 25, 1.5, 1, EV_HEARTBEAT), a);
    CHECK(n == 0, "machines must be independent");

    printf("7. shutdown clears what is still active\n");
    n = feed("crushing exits while alarmed",
             ev(M_CRUSHING, -1, ST_COMPLETED, cr->max_temperature + 1,
                cr->max_pressure + 1, 0, EV_SHUTDOWN), a);
    CHECK(n == 2 && a[0].kind == ALARM_CLEARED && a[1].kind == ALARM_CLEARED,
          "shutdown must clear active alarms");

    printf("8. bad machine id ignored\n");
    {
        MachineEvent bad = ev(0, 1, ST_ERROR, 99, 99, 99, EV_HEARTBEAT);
        bad.machine = 42;
        CHECK(qc_evaluate(&qc, &cfg, &bad, a) == 0, "invalid machine id");
    }

    CHECK(qc.raised_count == qc.cleared_count,
          "every raised alarm was cleared (%lu raised, %lu cleared)",
          qc.raised_count, qc.cleared_count);
    printf("  -> %lu readings, %lu raised, %lu cleared\n", qc.events_seen,
           qc.raised_count, qc.cleared_count);
}

/* Part 2: the real process behind pipes. */
static void process_test(void)
{
    int telem[2], alarm_p[2], fwd[2];
    pid_t pid;
    const MachineConfig *mx = &cfg.machine[M_MALAXATION];
    MachineEvent script[] = {
        ev(M_MALAXATION, 1, ST_OPERATING, 26, 1.5, 0.5, EV_BATCH_START),
        ev(M_SEPARATION, -1, ST_IDLE, 24, 0, 0, EV_HEARTBEAT),
        ev(M_MALAXATION, 1, ST_OPERATING, mx->max_temperature + 3, 1.5, 1,
           EV_HEARTBEAT),                                     /* RAISED   */
        ev(M_MALAXATION, 1, ST_PAUSED, mx->max_temperature + 2, 0, 1,
           EV_STATE_CHANGE),                                  /* (none)   */
        ev(M_MALAXATION, 1, ST_PAUSED, mx->recovery_temperature - 1, 0, 1,
           EV_HEARTBEAT),                                     /* CLEARED  */
        ev(M_SEPARATION, 1, ST_ERROR, 25, 0, 0.2, EV_STATE_CHANGE), /* RAISED */
        ev(M_SEPARATION, 1, ST_OPERATING, 25, 4, 0.2, EV_STATE_CHANGE), /* CLR */
        ev(M_MALAXATION, -1, ST_COMPLETED, 26, 0, 0, EV_SHUTDOWN),
        ev(M_SEPARATION, -1, ST_COMPLETED, 26, 0, 0, EV_SHUTDOWN),
    };
    const int expected[][3] = {
        { M_MALAXATION, FAULT_OVERHEAT,      ALARM_RAISED  },
        { M_MALAXATION, FAULT_OVERHEAT,      ALARM_CLEARED },
        { M_SEPARATION, FAULT_MACHINE_ERROR, ALARM_RAISED  },
        { M_SEPARATION, FAULT_MACHINE_ERROR, ALARM_CLEARED },
    };
    int nscript = (int)(sizeof script / sizeof script[0]);
    int nexp = (int)(sizeof expected / sizeof expected[0]);
    Alarm a;
    MachineEvent f;
    int got = 0, forwarded = 0, status;

    printf("9. quality_control_process behind pipes\n");
    if (pipe(telem) < 0 || pipe(alarm_p) < 0 || pipe(fwd) < 0) {
        perror("pipe");
        failures++;
        return;
    }
    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        failures++;
        return;
    }
    if (pid == 0) {
        close(telem[1]);            /* QC only reads telemetry ...        */
        close(alarm_p[0]);          /* ... and only writes alarms/status  */
        close(fwd[0]);
        _exit(quality_control_process(&cfg, telem[0], alarm_p[1], fwd[1]));
    }
    close(telem[0]);
    close(alarm_p[1]);
    close(fwd[1]);

    for (int i = 0; i < nscript; i++)
        write_full(telem[1], &script[i], sizeof script[i]);
    close(telem[1]);                /* last writer gone -> QC sees EOF   */

    while (read_full(alarm_p[0], &a, sizeof a) == (ssize_t)sizeof a) {
        if (got < nexp)
            CHECK(is(&a, expected[got][0], expected[got][1], expected[got][2]),
                  "alarm %d is %s/%s/%d", got, machine_name(a.machine),
                  fault_name(a.fault), a.kind);
        got++;
    }
    while (read_full(fwd[0], &f, sizeof f) == (ssize_t)sizeof f)
        forwarded++;
    close(alarm_p[0]);
    close(fwd[0]);

    waitpid(pid, &status, 0);
    CHECK(got == nexp, "expected %d alarms, got %d", nexp, got);
    CHECK(forwarded == nscript, "expected %d forwarded readings, got %d",
          nscript, forwarded);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "QC did not exit cleanly on EOF");
    printf("  -> %d alarms received, %d readings forwarded, QC exit %d\n",
           got, forwarded, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

int main(int argc, char **argv)
{
    char err[128];

    setvbuf(stdout, NULL, _IOLBF, 0);
    config_defaults(&cfg);
    if (argc > 1 && config_load(&cfg, argv[1]) != 0)
        return 2;
    if (config_validate(&cfg, err, sizeof err) != 0) {
        fprintf(stderr, "invalid config: %s\n", err);
        return 2;
    }
    printf("== Quality Control test ==\n");
    unit_tests();
    process_test();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
