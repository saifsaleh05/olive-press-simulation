#include "cleaning.h"
#include "common.h"
#include "config.h"
#include "ipc.h"
#include "placeholder.h"
#include "reception.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define STAGE_PIPE_COUNT 5

static volatile sig_atomic_t shutdown_requested = 0;

static void request_shutdown(int signal_number)
{
    (void)signal_number;
    shutdown_requested = 1;
}

static void reset_child_signals(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);

    action.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &action, NULL);
}

static int run_child(ProcessRole role, const Config *config, IpcPipe *pipes)
{
    int keep[2];
    size_t keep_count = 0;
    int result = 1;

    reset_child_signals();

    switch (role) {
    case ROLE_RECEPTION:
        keep[keep_count++] = pipes[0].write_fd;
        ipc_close_all_except(pipes, STAGE_PIPE_COUNT, keep, keep_count);
        result = run_reception(config, keep[0]);
        break;
    case ROLE_CLEANING:
        keep[keep_count++] = pipes[0].read_fd;
        keep[keep_count++] = pipes[1].write_fd;
        ipc_close_all_except(pipes, STAGE_PIPE_COUNT, keep, keep_count);
        result = run_cleaning(config, keep[0], keep[1]);
        break;
    case ROLE_WASHING:
        keep[keep_count++] = pipes[1].read_fd;
        keep[keep_count++] = pipes[2].write_fd;
        ipc_close_all_except(pipes, STAGE_PIPE_COUNT, keep, keep_count);
        result = run_passthrough_stage(role, STAGE_WASHING, keep[0], keep[1]);
        break;
    case ROLE_CRUSHING:
        keep[keep_count++] = pipes[2].read_fd;
        keep[keep_count++] = pipes[3].write_fd;
        ipc_close_all_except(pipes, STAGE_PIPE_COUNT, keep, keep_count);
        result = run_passthrough_stage(role, STAGE_CRUSHING, keep[0], keep[1]);
        break;
    case ROLE_MALAXATION:
        keep[keep_count++] = pipes[3].read_fd;
        keep[keep_count++] = pipes[4].write_fd;
        ipc_close_all_except(pipes, STAGE_PIPE_COUNT, keep, keep_count);
        result = run_passthrough_stage(role, STAGE_MALAXATION, keep[0], keep[1]);
        break;
    case ROLE_SEPARATION:
        keep[keep_count++] = pipes[4].read_fd;
        ipc_close_all_except(pipes, STAGE_PIPE_COUNT, keep, keep_count);
        result = run_separation_placeholder(keep[0]);
        break;
    case ROLE_QUALITY_CONTROL:
        ipc_close_all(pipes, STAGE_PIPE_COUNT);
        result = run_quality_control_placeholder();
        break;
    case ROLE_COUNT:
        ipc_close_all(pipes, STAGE_PIPE_COUNT);
        result = 1;
        break;
    }

    ipc_close_all(pipes, STAGE_PIPE_COUNT);
    return result;
}

static void terminate_started_children(ChildRecord *children, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (children[i].pid > 0 && !children[i].has_exited) {
            (void)kill(children[i].pid, SIGTERM);
        }
    }
}

static int wait_for_children(ChildRecord *children, size_t count)
{
    size_t remaining = count;
    int failure_count = 0;
    int termination_sent = 0;

    if (shutdown_requested) {
        terminate_started_children(children, count);
        termination_sent = 1;
    }

    while (remaining > 0) {
        int status;
        pid_t exited = waitpid(-1, &status, 0);
        size_t i;

        if (exited == -1) {
            if (errno == EINTR) {
                if (shutdown_requested && !termination_sent) {
                    terminate_started_children(children, count);
                    termination_sent = 1;
                }
                continue;
            }
            perror("Supervisor: waitpid");
            return -1;
        }

        for (i = 0; i < count; ++i) {
            if (children[i].pid == exited) {
                children[i].wait_status = status;
                children[i].has_exited = 1;
                --remaining;
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    ++failure_count;
                }
                break;
            }
        }

        if (shutdown_requested && !termination_sent) {
            terminate_started_children(children, count);
            termination_sent = 1;
        }
    }
    return failure_count;
}

static void print_child_table(const ChildRecord *children, size_t count)
{
    size_t i;
    puts("\nSupervisor child-process table");
    puts("-------------------------------------------------");
    puts("Role                 PID       Exit result");
    puts("-------------------------------------------------");
    for (i = 0; i < count; ++i) {
        char result[32];
        if (!children[i].has_exited) {
            snprintf(result, sizeof(result), "still running");
        } else if (WIFEXITED(children[i].wait_status)) {
            snprintf(result, sizeof(result), "exit %d",
                     WEXITSTATUS(children[i].wait_status));
        } else if (WIFSIGNALED(children[i].wait_status)) {
            snprintf(result, sizeof(result), "signal %d",
                     WTERMSIG(children[i].wait_status));
        } else {
            snprintf(result, sizeof(result), "other");
        }
        printf("%-20s %-9ld %s\n",
               process_role_name(children[i].role),
               (long)children[i].pid, result);
    }
}

int main(void)
{
    Config config;
    IpcPipe pipes[STAGE_PIPE_COUNT];
    ChildRecord children[ROLE_COUNT];
    char config_error[128];
    size_t started = 0;
    size_t i;
    int failure_count;
    struct sigaction action;

    setvbuf(stdout, NULL, _IOLBF, 0);

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_shutdown;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        perror("Supervisor: install signal handlers");
        return EXIT_FAILURE;
    }

    config_set_defaults(&config);
    if (config_validate(&config, config_error, sizeof(config_error)) == -1) {
        fprintf(stderr, "Invalid configuration: %s\n", config_error);
        return EXIT_FAILURE;
    }
    if (ipc_create_pipes(pipes, STAGE_PIPE_COUNT) == -1) {
        perror("Supervisor: create stage pipes");
        return EXIT_FAILURE;
    }

    memset(children, 0, sizeof(children));
    for (i = 0; i < ROLE_COUNT; ++i) {
        pid_t pid;
        children[i].role = (ProcessRole)i;
        children[i].pid = -1;

        pid = fork();
        if (pid == -1) {
            perror("Supervisor: fork");
            ipc_close_all(pipes, STAGE_PIPE_COUNT);
            terminate_started_children(children, started);
            (void)wait_for_children(children, started);
            print_child_table(children, started);
            return EXIT_FAILURE;
        }
        if (pid == 0) {
            int result = run_child((ProcessRole)i, &config, pipes);
            _exit(result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
        }

        children[i].pid = pid;
        ++started;
        printf("[Supervisor] Started %-15s pid=%ld\n",
               process_role_name((ProcessRole)i), (long)pid);

        if (shutdown_requested) {
            fprintf(stderr, "Supervisor: shutdown requested during startup\n");
            ipc_close_all(pipes, STAGE_PIPE_COUNT);
            terminate_started_children(children, started);
            (void)wait_for_children(children, started);
            print_child_table(children, started);
            return EXIT_FAILURE;
        }
    }

    ipc_close_all(pipes, STAGE_PIPE_COUNT);

    failure_count = wait_for_children(children, ROLE_COUNT);
    print_child_table(children, ROLE_COUNT);

    if (shutdown_requested) {
        fprintf(stderr, "Supervisor: shutdown requested by signal\n");
        return EXIT_FAILURE;
    }
    if (failure_count != 0) {
        fprintf(stderr, "Supervisor: %d child process(es) failed\n",
                failure_count);
        return EXIT_FAILURE;
    }

    puts("Supervisor: all child processes exited cleanly");
    return EXIT_SUCCESS;
}
