#include "ipc.h"

#include <errno.h>
#include <unistd.h>

ssize_t read_full(int fd, void *buffer, size_t count)
{
    unsigned char *cursor = buffer;
    size_t total = 0;

    while (total < count) {
        ssize_t received = read(fd, cursor + total, count - total);
        if (received > 0) {
            total += (size_t)received;
            continue;
        }
        if (received == 0) {
            if (total == 0) {
                return 0;
            }
            errno = EPROTO;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    return (ssize_t)total;
}

ssize_t write_full(int fd, const void *buffer, size_t count)
{
    const unsigned char *cursor = buffer;
    size_t total = 0;

    while (total < count) {
        ssize_t sent = write(fd, cursor + total, count - total);
        if (sent > 0) {
            total += (size_t)sent;
            continue;
        }
        if (sent == 0) {
            errno = EIO;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    return (ssize_t)total;
}

int ipc_create_pipes(IpcPipe *pipes, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        int descriptors[2];
        pipes[i].read_fd = -1;
        pipes[i].write_fd = -1;
        if (pipe(descriptors) == -1) {
            ipc_close_all(pipes, i);
            return -1;
        }
        pipes[i].read_fd = descriptors[0];
        pipes[i].write_fd = descriptors[1];
    }
    return 0;
}

void ipc_close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0) {
        (void)close(*fd);
        *fd = -1;
    }
}

void ipc_close_all(IpcPipe *pipes, size_t count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        ipc_close_fd(&pipes[i].read_fd);
        ipc_close_fd(&pipes[i].write_fd);
    }
}

static int fd_is_kept(int fd, const int *keep_fds, size_t keep_count)
{
    size_t i;
    for (i = 0; i < keep_count; ++i) {
        if (fd == keep_fds[i]) {
            return 1;
        }
    }
    return 0;
}

void ipc_close_all_except(IpcPipe *pipes, size_t count,
                          const int *keep_fds, size_t keep_count)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (!fd_is_kept(pipes[i].read_fd, keep_fds, keep_count)) {
            ipc_close_fd(&pipes[i].read_fd);
        }
        if (!fd_is_kept(pipes[i].write_fd, keep_fds, keep_count)) {
            ipc_close_fd(&pipes[i].write_fd);
        }
    }
}
