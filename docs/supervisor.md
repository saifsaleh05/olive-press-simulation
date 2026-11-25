# Supervisor, IPC and GUI notes

Owner: Ameer. These are the parts the rest of the team plugs into.

## Processes and channels

```
                 SIGUSR1 / SIGUSR2 / SIGRTMIN / SIGRTMIN+1 / SIGTERM
          +-------------------------------------------------------------+
          |                                                             v
   +------------+   batch   +---------+   batch   +---------+  batch  +----------+
   | Reception  | --------> |Cleaning | --------> | Washing | ------> | Crushing |
   +------------+   pipe 0  +---------+   pipe 1  +---------+ pipe 2  +----------+
                                                                          | pipe 3
   +------------+   batch   +------------+                                v
   | Separation | <-------- | Malaxation | <-----------------------------+
   +------------+   pipe 4  +------------+
        |
        | ProductionResult (result pipe)
        v
   +------------------------------+   MachineEvent copies   +-----------------+
   |  Supervisor (parent, OpenGL) | ----------------------> | Quality Control |
   |                              | <---------------------- |                 |
   +------------------------------+     Alarm (alarm pipe)  +-----------------+
        ^
        | MachineEvent (telemetry pipe, shared by all seven children)
```

* **Five batch pipes** carry `Batch` structs stage to stage. When reception has
  sent its last batch it closes its write end; each stage sees EOF, closes its
  own output and exits. That is how the line drains and finishes by itself.
* **Telemetry pipe**: every child writes `MachineEvent`s to one shared pipe.
  Each struct is far below `PIPE_BUF` (4096 bytes on Linux), so `write()` is
  atomic and messages from different children never interleave.
  `tests/test_ipc.c` checks this with six writers and 12000 messages.
* **QC feed**: the supervisor forwards each stage event to QC. QC therefore
  needs no pipe to every machine, and the parent is the only reader of the
  telemetry pipe. The write end is non blocking: if QC falls behind, samples
  are dropped and counted (`QC samples lost` in the GUI) instead of freezing
  the window.
* **Alarm pipe**: QC -> supervisor.
* **Result pipe**: separation -> supervisor, one `ProductionResult` per batch.

Why pipes and not FIFOs: every process is a child of the supervisor, so
anonymous pipes created before `fork()` are enough. They need no names in the
filesystem and nothing to clean up after a crash.

Why signals for control: commands are rare, carry no data, and must reach a
child even while it sits in `poll()`/`nanosleep()`. The handlers only set a
`sig_atomic_t` flag; the child's loop acts on it.

## Message contract (`include/common.h`)

| struct             | written by          | read by          | when                         |
|--------------------|---------------------|------------------|------------------------------|
| `Batch`            | stage *i*           | stage *i+1*      | batch done at stage *i*      |
| `MachineEvent`     | every child         | supervisor (+QC) | every `tick`, and on changes |
| `Alarm`            | QC                  | supervisor       | value crosses a limit        |
| `ProductionResult` | separation          | supervisor       | one per finished batch       |

`MachineEvent.event` says why it was sent: `EV_TELEMETRY` (periodic),
`EV_STATE_CHANGE`, `EV_BATCH_IN`, `EV_BATCH_OUT`, `EV_EXIT`. The supervisor
counts `BATCH_IN`/`BATCH_OUT` per machine; the difference between one
machine's `out` and the next one's `in` is the number of batches waiting in
that pipe, which is what the GUI draws on the arrows.

`ProductionResult.original_mass` was added to the proposed contract so the
supervisor can check mass conservation per batch:
`oil + water + pomace + losses == original_mass`.

Always use `read_full()` / `write_full()` for these structs. They retry on
`EINTR` and short transfers, and `read_full()` returns 0 only on a clean EOF.

## Control signals (`include/ipc.h`)

| macro        | signal       | child must...                                   |
|--------------|--------------|-------------------------------------------------|
| `SIG_PAUSE`  | `SIGUSR1`    | stop working, report `ST_PAUSED`                |
| `SIG_RESUME` | `SIGUSR2`    | go back to the state before the pause           |
| `SIG_FAULT`  | `SIGRTMIN`   | report `ST_ERROR`, stop heating, hold the batch |
| `SIG_CLEAR`  | `SIGRTMIN+1` | leave `ST_ERROR` and continue                   |
| -            | `SIGTERM`    | close pipes, send a last event, exit 0          |

Real-time signals are used for fault/clear because standard signals do not
queue: a fault and a clear sent close together could merge into one delivery.
`SIGUSR1`/`SIGUSR2` were the obvious pair for pause/resume.

Handlers are installed **without** `SA_RESTART`, so a signal wakes the child
out of `poll()` or `nanosleep()` immediately.

## Child setup after fork()

Done in `supervisor_start()` before calling the stage function:

1. Every file descriptor except the ones in that child's `StageCtx` is closed,
   including the X11 socket when the GUI is running. If a child kept a stray
   write end of a batch pipe, the next stage would never see EOF.
2. `SIGINT` is ignored (Ctrl-C belongs to the supervisor, which then shuts
   the children down in order) and `SIGPIPE` is ignored (`EPIPE` is reported
   instead of killing the process).
3. The child leaves with `_exit()`, so freeglut's `atexit` handlers and the
   parent's stdio buffers are not run twice.

## Fault response

* QC alarm -> supervisor sends `SIG_FAULT` to that machine and holds reception
  with `SIG_PAUSE` so no new olives enter while a machine is down. Batches
  already in the line keep draining.
* `auto_recover = 1`: once the faulted machine reports a temperature below
  `max_temp * recover_ratio` (or pressure below `max_pressure * recover_ratio`)
  the supervisor sends `SIG_CLEAR` and releases reception. Manual faults are
  only cleared by the operator.
* A child that dies unexpectedly (non zero exit or killed by a signal) is
  found by `waitpid(WNOHANG)`. It is shown as ERROR / "process died", and the
  rest of the line is stopped safely, since a pipeline with a hole in it cannot
  continue.
* Stop: `SIGTERM` to all children, `SIGKILL` to anyone still alive after 2 s.

## GUI data flow

```
GLUT timer (every 33 ms)
    supervisor_poll(0)        poll() with zero timeout, drain up to 512 msgs,
                              waitpid(WNOHANG), auto recovery
    supervisor_snapshot(&s)   copy of the whole FactorySnapshot
    glutPostRedisplay()
display()                     draws only from the snapshot
```

No GLUT callback ever calls a blocking `read()`. If nothing has arrived, the
frame just redraws the last known values. The machine colours come from the
state each child reported, so the screen shows real events only.

## Supervisor API (`processes/supervisor.h`)

| function                       | blocking | notes                                         |
|--------------------------------|----------|-----------------------------------------------|
| `supervisor_init(cfg)`         | no       | once, before anything else                    |
| `supervisor_start()`           | no       | pipes + 7 forks; `LINE_READY -> RUNNING`      |
| `supervisor_poll(ms)`          | up to ms | GUI passes 0, headless passes `tick`          |
| `supervisor_pause/resume()`    | no       |                                               |
| `supervisor_inject_fault(id)`  | no       | id 0..5                                       |
| `supervisor_clear_fault(id)`   | no       |                                               |
| `supervisor_stop()`            | no       | starts the shutdown                           |
| `supervisor_shutdown()`        | <= ~2 s  | stop and wait for every child; call on exit   |
| `supervisor_snapshot(&out)`    | no       | copy for the GUI                              |
| `supervisor_log(fmt, ...)`     | no       | timestamped, to stdout and the GUI event log  |

## Notes for the team

* **Saif**: startup currently lives in `supervisor_start()` because the
  supervisor needs the PIDs and the parent pipe ends. If you want it in its
  own file, keep that function's fd closing and signal setup as they are.
* **Ibrahim**: QC only has to read `MachineEvent`s from `ctx->qc_in_fd` and
  write `Alarm`s to `ctx->alarm_fd`. The supervisor already does the
  pause/fault/clear side. If you add more recovery states, tell me so the GUI
  can show them.
* **Yanal**: the GUI shows `temperature`, `pressure`, `progress` and the
  limits from `Config`. Any new config field needs a line in `set_key()` in
  `common/config.c`.
* Everyone: implement `xxx_run(const StageCtx *ctx)` from `include/ipc.h` in
  `processes/xxx.c`. The mock in `processes/mock_stages.c` is a working
  reference for the loop (poll the input with a `tick` timeout, report
  telemetry while idle, honour the signal flags).
