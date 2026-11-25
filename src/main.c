#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "config.h"
#include "graphics.h"
#include "supervisor.h"

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [config-file] [--headless] [--autostart]\n"
            "\n"
            "  config-file   key = value settings, see config/default.cfg\n"
            "  --headless    no window; run the line, print progress, read\n"
            "                commands from stdin (p, r, f N, c N, x)\n"
            "  --autostart   GUI only: start the line without pressing S\n",
            prog);
}

static void on_stop_signal(int sig)
{
    (void)sig;
    supervisor_request_stop();
}

static void install_parent_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void headless_command(char *line)
{
    int id = -1;
    char cmd = line[0];

    if (sscanf(line + 1, "%d", &id) == 1)
        id -= 1;    /* operators count machines from 1 */

    switch (cmd) {
    case 'p': supervisor_pause(); break;
    case 'r': supervisor_resume(); break;
    case 'f': supervisor_inject_fault(id); break;
    case 'c': supervisor_clear_fault(id); break;
    case 'x':
    case 'q': supervisor_stop(); break;
    case '\n':
    case '\0': break;
    default:  fprintf(stderr, "unknown command: %s", line);
    }
}

static void print_status(const FactorySnapshot *s)
{
    printf("[%6.2f] status:", s->uptime);
    for (int id = 0; id < NUM_MACHINES; id++)
        printf(" %.4s=%s", machine_name(id), state_name(s->m[id].state));
    printf(" | done %d\n", s->produced);
    fflush(stdout);
}

static int run_headless(const Config *cfg)
{
    FactorySnapshot s;
    int    stdin_open = 1;
    double next_status = 1.0;

    if (supervisor_start() < 0)
        return 1;

    while (!supervisor_finished()) {
        if (stdin_open) {
            struct pollfd p = { .fd = STDIN_FILENO, .events = POLLIN };
            if (poll(&p, 1, 0) > 0) {
                char line[64];
                if (fgets(line, sizeof line, stdin))
                    headless_command(line);
                else
                    stdin_open = 0;
            }
        }

        supervisor_poll((int)(cfg->tick * 1000));

        supervisor_snapshot(&s);
        if (s.uptime >= next_status) {
            print_status(&s);
            next_status += 2.0;
        }
    }

    supervisor_shutdown();
    supervisor_snapshot(&s);

    printf("\nsummary: %d/%d batches, olives %.1f kg, oil %.1f kg, water %.1f kg, "
           "pomace %.1f kg, debris %.1f kg, alarms %d, balance error %.2e kg\n",
           s.produced, cfg->num_batches, s.mass_in, s.oil, s.water, s.pomace, s.losses,
           s.alarms, s.balance_error);

    return s.produced == cfg->num_batches && s.balance_error < 1e-6 ? 0 : 2;
}

int main(int argc, char **argv)
{
    Config      cfg;
    const char *cfg_path = NULL;
    int         headless = 0, autostart = 0, rc;
    char        err[256];

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless"))
            headless = 1;
        else if (!strcmp(argv[i], "--autostart"))
            autostart = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        } else {
            cfg_path = argv[i];
        }
    }

    config_defaults(&cfg);
    if (cfg_path && config_load(cfg_path, &cfg, err, sizeof err) < 0) {
        fprintf(stderr, "config: %s\n", err);
        return 1;
    }

    supervisor_init(&cfg);
    install_parent_signals();

    if (headless)
        return run_headless(&cfg);

    rc = graphics_run(&argc, argv, &cfg, autostart);
    /* window closed or Q pressed: make sure no child outlives us */
    supervisor_shutdown();
    return rc;
}
