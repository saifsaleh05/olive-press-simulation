#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "ipc.h"
#include "processes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { NBATCH = 3 };
static int feed_p[2], w2c_p[2], out_p[2], telem_p[2];
static Config cfg;

static void close_pair(int p[2])
{
    close_fd(&p[0]);
    close_fd(&p[1]);
}

static pid_t spawn_washing(void)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    close_fd(&feed_p[1]);
    close_fd(&w2c_p[0]);
    close_pair(out_p);
    close_fd(&telem_p[0]);
    _exit(washing_process(&cfg, feed_p[0], w2c_p[1], telem_p[1]));
}

static pid_t spawn_crushing(void)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    close_pair(feed_p);
    close_fd(&w2c_p[1]);
    close_fd(&out_p[0]);
    close_fd(&telem_p[0]);
    _exit(crushing_process(&cfg, w2c_p[0], out_p[1], telem_p[1]));
}

int main(int argc, char **argv)
{
    Batch in[NBATCH], got;
    pid_t washer, crusher;
    int status, seen = 0, starts[2] = {0}, done[2] = {0};
    MachineEvent ev;

    config_defaults(&cfg);
    if (argc > 1 && config_load(&cfg, argv[1]) != 0)
        return 1;
    cfg.auto_faults_enabled = 0;
    cfg.tick_ms = 5;
    cfg.machine[M_WASHING].base_time = 0.03;
    cfg.machine[M_CRUSHING].base_time = 0.04;

    if (pipe(feed_p) || pipe(w2c_p) || pipe(out_p) || pipe(telem_p)) {
        perror("pipe");
        return 1;
    }
    washer = spawn_washing();
    crusher = spawn_crushing();
    if (washer < 0 || crusher < 0) {
        perror("fork");
        return 1;
    }

    close_fd(&feed_p[0]);
    close_pair(w2c_p);
    close_fd(&out_p[1]);
    close_fd(&telem_p[1]);

    for (int i = 0; i < NBATCH; i++) {
        in[i] = (Batch){
            .id = 100 + i,
            .original_mass = 20.0 + 5.0 * i,
            .current_mass = 20.0 + 5.0 * i,
            .impurity_fraction = 0.02,
            .oil_potential = 0.20,
            .quality = 0.90,
            .stage = M_CLEANING
        };
        if (write_full(feed_p[1], &in[i], sizeof in[i]) < 0)
            return 1;
    }
    close_fd(&feed_p[1]);

    while (read_full(out_p[0], &got, sizeof got) == (ssize_t)sizeof got) {
        if (seen >= NBATCH)
            return 1;
        double min_mass = in[seen].current_mass * (1.0 - cfg.washing_loss.max);
        double max_mass = in[seen].current_mass * (1.0 - cfg.washing_loss.min);

        printf("[mock malaxation] received batch %d: %.2f kg paste\n",
               got.id, got.current_mass);
        if (got.id != in[seen].id ||
            got.stage != M_CRUSHING || got.current_mass < min_mass - 1e-9 ||
            got.current_mass > max_mass + 1e-9 ||
            got.impurity_fraction > in[seen].impurity_fraction)
            return 1;
        seen++;
    }
    close_fd(&out_p[0]);

    while (read_full(telem_p[0], &ev, sizeof ev) == (ssize_t)sizeof ev) {
        int idx = ev.machine == M_WASHING ? 0 :
                  ev.machine == M_CRUSHING ? 1 : -1;
        if (idx >= 0 && ev.event == EV_BATCH_START)
            starts[idx]++;
        if (idx >= 0 && ev.event == EV_BATCH_DONE)
            done[idx]++;
    }
    close_fd(&telem_p[0]);
    waitpid(washer, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 1;
    waitpid(crusher, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 1;

    if (seen != NBATCH || starts[0] != NBATCH || starts[1] != NBATCH ||
        done[0] != NBATCH || done[1] != NBATCH)
        return 1;
    printf("test_wash_crush: PASS (%d batches, telemetry verified)\n", seen);
    return 0;
}
