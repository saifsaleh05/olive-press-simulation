#ifndef OLIVE_IPC_H
#define OLIVE_IPC_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int read_fd;
    int write_fd;
} IpcPipe;

/*
 * Returns count on success, 0 on clean EOF before any bytes, and -1 on error.
 * EOF after a partial fixed-size message is reported as EPROTO.
 */
ssize_t read_full(int fd, void *buffer, size_t count);
ssize_t write_full(int fd, const void *buffer, size_t count);

int ipc_create_pipes(IpcPipe *pipes, size_t count);
void ipc_close_fd(int *fd);
void ipc_close_all(IpcPipe *pipes, size_t count);
void ipc_close_all_except(IpcPipe *pipes, size_t count,
                          const int *keep_fds, size_t keep_count);

#endif
