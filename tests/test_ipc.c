#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(EXIT_FAILURE);
}

int main(void)
{
    IpcPipe channel;
    IpcPipe truncated_channel;
    pid_t child;
    unsigned char sent[4096];
    unsigned char received[4096];
    size_t i;
    int status;

    for (i = 0; i < sizeof(sent); ++i) {
        sent[i] = (unsigned char)(i % 251U);
    }
    if (ipc_create_pipes(&channel, 1) == -1) {
        fail("pipe creation failed");
    }

    child = fork();
    if (child == -1) {
        fail("fork failed");
    }
    if (child == 0) {
        ipc_close_fd(&channel.read_fd);
        if (write_full(channel.write_fd, sent, sizeof(sent)) !=
            (ssize_t)sizeof(sent)) {
            _exit(EXIT_FAILURE);
        }
        ipc_close_fd(&channel.write_fd);
        _exit(EXIT_SUCCESS);
    }

    ipc_close_fd(&channel.write_fd);
    if (read_full(channel.read_fd, received, sizeof(received)) !=
        (ssize_t)sizeof(received)) {
        fail("read_full did not receive the complete message");
    }
    if (memcmp(sent, received, sizeof(sent)) != 0) {
        fail("message contents changed in transit");
    }
    if (read_full(channel.read_fd, received, sizeof(received)) != 0) {
        fail("clean EOF was not reported");
    }
    ipc_close_fd(&channel.read_fd);

    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail("writer child did not exit cleanly");
    }

    if (ipc_create_pipes(&truncated_channel, 1) == -1) {
        fail("truncated-message pipe creation failed");
    }
    if (write_full(truncated_channel.write_fd, sent, sizeof(sent) / 2) !=
        (ssize_t)(sizeof(sent) / 2)) {
        fail("could not prepare truncated message");
    }
    ipc_close_fd(&truncated_channel.write_fd);
    errno = 0;
    if (read_full(truncated_channel.read_fd, received, sizeof(received)) != -1 ||
        errno != EPROTO) {
        fail("partial EOF was not reported as a protocol error");
    }
    ipc_close_fd(&truncated_channel.read_fd);

    puts("PASS: fixed-size IPC transfer, clean EOF, and truncated EOF behavior");
    return EXIT_SUCCESS;
}
