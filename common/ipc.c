/* ipc.c - fixed-size message helpers for pipes. See ipc.h. */
#include "ipc.h"

#include <errno.h>
#include <unistd.h>

ssize_t write_full(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)w;
    }
    return (ssize_t)n;
}

ssize_t read_full(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0) {               /* EOF */
            if (done == 0)
                return 0;
            errno = EPIPE;          /* truncated message */
            return -1;
        }
        done += (size_t)r;
    }
    return (ssize_t)n;
}

void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}
