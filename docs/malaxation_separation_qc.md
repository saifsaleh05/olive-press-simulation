# Malaxation, Separation & Quality Control

Owner: Ibrahim. Files: `processes/malaxation.c`, `processes/separation.c`,
`processes/quality_control.c`, the shared run-time in `common/simulation.c`,
and the tests in `tests/`.

## Channels I use

```
Crushing --Batch--> Malaxation --Batch--> Separation --ProductionResult--> Supervisor
                         |                     |
                         +----MachineEvent-----+--> (shared telemetry pipe, 6 writers)
                                                         |
                                                  Quality Control --Alarm--> Supervisor
                                                         \--MachineEvent copy (optional)--> Supervisor (GUI)
```

Every message is a fixed-size struct from `include/common.h`, sent with
`write_full()` and read with `read_full()`. All messages are far smaller than
`PIPE_BUF` (4096 B), so each write is atomic. That is why six machines can
share one telemetry pipe without their messages getting mixed up.

| Message | Size | Writer → Reader |
|---|---|---|
| `Batch` | 56 B | stage → next stage |
| `MachineEvent` | 64 B | each machine → QC (→ Supervisor if forwarded) |
| `Alarm` | 32 B | QC → Supervisor |
| `ProductionResult` | 64 B | Separation → Supervisor |

## Functions (see `include/processes.h`)

| Function | What it does |
|---|---|
| `malaxation_process(cfg, in_fd, out_fd, telem_fd)` | child body of Malaxation |
| `separation_process(cfg, in_fd, result_fd, telem_fd)` | child body of Separation |
| `separation_compute(batch, cfg, water_u, &result)` | pure three-phase mass balance |
| `quality_control_process(cfg, telem_fd, alarm_fd, forward_fd)` | child body of QC |
| `qc_init(&qc)` / `qc_evaluate(&qc, cfg, &event, alarms)` | pure QC rules, used by the process and the tests |
| `machine_run_stage(...)` (`common/simulation.c`) | tick loop that any stage can reuse: pause, fault, model, telemetry, EOF |

Every `*_process` function closes the fds it received before it returns. The
caller (`main.c`) must close every other pipe end in that child after
`fork()`. Otherwise EOF never reaches the reader.

## Malaxation

- Mixing time = `MALAXATION_BASE_TIME × kg / NOMINAL_BATCH_KG`.
- Temperature: `T += (H − C·(T − T_ambient))·dt`, with `H` applied only while
  running. Pressure follows `BASE_PRESSURE + PRESSURE_GAIN·load`.
- Above `MALAXATION_MAX_TEMPERATURE` the paste loses `QUALITY_LOSS_RATE`
  quality per second, which lowers the oil yield in Separation.
- The paste mass is not changed here.

## Separation: three-phase mass balance

```
paste      = batch.current_mass               (after Cleaning/Washing losses)
losses     = batch.original_mass − paste
efficiency = EXTRACTION_EFFICIENCY × (1 − QUALITY_WEIGHT + QUALITY_WEIGHT × quality)
oil        = paste × oil_potential × efficiency
water      = paste × U(VEG_WATER_PERCENT_MIN, VEG_WATER_PERCENT_MAX)
pomace     = paste − oil − water          (unrecovered oil stays in the pomace)
```

Because `pomace` is computed from the other two, `paste = oil + water + pomace`
holds by construction, and `original = losses + oil + water + pomace`.
`test_separation_mass` checks this on 2,000,000 batches; the largest relative
error is about 1.8e-16. Out-of-range inputs are clamped, so no output can be
negative.

## Why pause/resume never loses a batch

The batch is a local variable of the machine process. `SIG_PAUSE` and
`SIG_FAULT` only set a `sig_atomic_t` flag. On the next tick the loop sees the
flag, sets the state to PAUSED or ERROR, and stops advancing `progress`. It
also stops reading new batches, so upstream backs up behind the pipe. The
machine keeps cooling and keeps sending telemetry. `SIG_RESUME` clears the
flags, and the same batch continues from the same progress.
`test_malax_sep` checks this: progress stays at the same value for all 29
PAUSED readings.

## QC is independent

QC is its own process. It never sees a batch, only telemetry. It decides
alarms from the limits in `config.txt` alone and does not depend on the
Supervisor's state. It ends by itself when the last machine closes the
telemetry pipe (EOF), or on `SIGTERM`. See `docs/qc_alarm_table.md`.

## Running the tests (Linux / WSL)

```
make test
```

| Test | Shows |
|---|---|
| `test_separation_mass` | mass balance after earlier losses, edge cases, OpenMP sequential vs parallel timing |
| `test_quality_control` | normal readings, each violation, no duplicates, hysteresis recovery, shutdown; then the real QC process behind pipes, exiting on EOF |
| `test_malax_sep` | real Malaxation, Separation and QC processes with 6 different batches: operator pause/resume, injected failure + repair, injected overheat → automatic pause → recovery → resume, ordered results, mass balance, clean exits |

## Open points for the team

- **Queue capacity (`QUEUE_CAPACITY=2`)**: a pipe holds about 1000 batches,
  so the agreed limit of 2 waiting batches still needs a credit/ack mechanism
  (Saif). The stage loop blocks in `write_full()` when downstream is full, so
  it works with either approach.
- **Status feed for the GUI**: `quality_control_process` can forward every
  `MachineEvent` to the Supervisor (`forward_fd`), so the Supervisor needs
  only one status pipe. Machines can also write a second pipe directly. Saif
  and Ameer to decide.
- `main.c` / Supervisor: create the pipes, fork, close unused ends, and send
  `SIG_PAUSE`/`SIG_RESUME` in response to alarms as listed in the table.
