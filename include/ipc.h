/*
 * ipc.h - pipe helpers and the signal protocol.
 *
 * SHARED CONTRACT.
 *
 * Channels (all anonymous pipes created by the Supervisor before fork()):
 *   stage pipes (5)   Reception -> Cleaning -> Washing -> Crushing
 *                     -> Malaxation -> Separation         : Batch
 *   telemetry pipe    every machine (6 writers) -> QC     : MachineEvent
 *   alarm pipe        QC -> Supervisor                    : Alarm
 *   result pipe       Separation -> Supervisor            : ProductionResult
 *
 * Every message is a fixed-size struct smaller than PIPE_BUF (4096 bytes on
 * Linux), so a single write() of one message is atomic: the six machines can
 * share the telemetry pipe without their messages being interleaved.
 *
 * EOF rule: after fork() every process must close every pipe end it does not
 * use. A reader only sees EOF when ALL write ends are closed, so a forgotten
 * write end (in the Supervisor or a sibling) keeps the reader alive forever.
 *
 * Signals (Supervisor -> machine). Handlers only set sig_atomic_t flags; the
 * machine acts on them at its next tick.
 *   SIG_PAUSE    (SIGUSR1)    pause: keep the current batch, stop progressing,
 *                             accept no new batch, cool down
 *   SIG_RESUME   (SIGUSR2)    resume after a pause, or clear a failure
 *   SIG_FAULT    (SIGRTMIN+1) manual injection: machine failure (ST_ERROR)
 *   SIG_HEAT     (SIGRTMIN+2) manual injection: temperature += HEAT_SPIKE
 *   SIG_PRESSURE (SIGRTMIN+3) manual injection: pressure += PRESSURE_SPIKE
 *   SIGTERM                   graceful shutdown
 */
#ifndef IPC_H
#define IPC_H

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>

#define SIG_PAUSE  SIGUSR1
#define SIG_RESUME SIGUSR2
#define SIG_FAULT    (SIGRTMIN + 1)
#define SIG_HEAT     (SIGRTMIN + 2)
#define SIG_PRESSURE (SIGRTMIN + 3)

/* Write exactly n bytes, retrying on EINTR and short writes.
 * Returns n on success, -1 on error (errno set; EPIPE if the reader is gone). */
ssize_t write_full(int fd, const void *buf, size_t n);

/* Read exactly n bytes, retrying on EINTR and short reads.
 * Returns n on success, 0 on clean EOF before any byte, -1 on error or on
 * EOF in the middle of a message (errno = EPIPE). */
ssize_t read_full(int fd, void *buf, size_t n);

/* close() that ignores fd < 0 and sets *fd = -1. */
void close_fd(int *fd);

#endif /* IPC_H */
