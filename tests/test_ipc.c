#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "ipc.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* Writer sends one message a few bytes at a time; reader must get it whole. */
static void test_split_message(void)
{
    int p[2];
    MachineEvent in = { .machine = M_CRUSHING, .batch_id = 42, .state = ST_OPERATING,
                        .temperature = 31.5, .pressure = 3.1, .progress = 0.5 };
    MachineEvent out;

    CHECK(pipe(p) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        const char *b = (const char *)&in;
        close(p[0]);
        for (size_t i = 0; i < sizeof in; i += 5) {
            size_t n = sizeof in - i < 5 ? sizeof in - i : 5;
            if (write(p[1], b + i, n) < 0)
                _exit(1);
            usleep(200);
        }
        _exit(0);
    }
    close(p[1]);
    CHECK(read_full(p[0], &out, sizeof out) == (ssize_t)sizeof out);
    CHECK(memcmp(&in, &out, sizeof in) == 0);
    CHECK(read_full(p[0], &out, sizeof out) == 0);     /* clean EOF */
    close(p[0]);
    waitpid(pid, NULL, 0);
}

/* EOF half way through a message is an error, not a short success. */
static void test_truncated_message(void)
{
    int   p[2];
    Batch b;
    char  half[sizeof b / 2];

    memset(half, 7, sizeof half);
    CHECK(pipe(p) == 0);
    CHECK(write(p[1], half, sizeof half) == (ssize_t)sizeof half);
    close(p[1]);
    errno = 0;
    CHECK(read_full(p[0], &b, sizeof b) == -1);
    CHECK(errno == EPIPE);
    close(p[0]);
}

static void on_alarm(int sig) { (void)sig; }

/* A signal arriving while read() blocks must not lose the message. */
static void test_eintr(void)
{
    int p[2];
    struct sigaction sa;
    ProductionResult in = { .batch_id = 3, .original_mass = 500, .oil = 80 }, out;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;       /* no SA_RESTART on purpose */
    sigaction(SIGALRM, &sa, NULL);

    CHECK(pipe(p) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        usleep(150000);
        kill(getppid(), SIGALRM);
        usleep(150000);
        write_full(p[1], &in, sizeof in);
        _exit(0);
    }
    close(p[1]);
    CHECK(read_full(p[0], &out, sizeof out) == (ssize_t)sizeof out);
    CHECK(out.batch_id == 3 && out.oil == 80);
    close(p[0]);
    waitpid(pid, NULL, 0);
}

/* Several writers on one pipe: messages under PIPE_BUF never interleave. */
static void test_many_writers(void)
{
    enum { WRITERS = 6, EACH = 2000 };
    int p[2], count[WRITERS] = { 0 }, total = 0, bad = 0;
    MachineEvent ev;

    CHECK(pipe(p) == 0);
    for (int w = 0; w < WRITERS; w++) {
        if (fork() == 0) {
            close(p[0]);
            for (int i = 0; i < EACH; i++) {
                MachineEvent e = { .machine = w, .batch_id = i, .temperature = w * 1000.0 + i };
                write_full(p[1], &e, sizeof e);
            }
            _exit(0);
        }
    }
    close(p[1]);
    while (read_full(p[0], &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.machine < 0 || ev.machine >= WRITERS ||
            ev.temperature != ev.machine * 1000.0 + ev.batch_id ||
            ev.batch_id != count[ev.machine])
            bad++;
        else
            count[ev.machine]++;
        total++;
    }
    close(p[0]);
    while (wait(NULL) > 0)
        ;
    CHECK(bad == 0);
    CHECK(total == WRITERS * EACH);
}

int main(void)
{
    test_split_message();
    test_truncated_message();
    test_eintr();
    test_many_writers();

    if (failures) {
        fprintf(stderr, "test_ipc: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_ipc: ok\n");
    return 0;
}
