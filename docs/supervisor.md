# Supervisor, IPC and GUI design

## Process and channel topology

```text
Reception -> Cleaning -> Washing -> Crushing -> Malaxation -> Separation
     \           \          \          \             \             \
      +-----------+----------+----------+-------------+-- telemetry --> QC
                                                                  |      |
                                                     status copies|      |alarms
                                                                  v      v
                                                              Supervisor

Separation -------------------- ProductionResult --------------------^
Supervisor -------------------- control signals ----------------> machines
```

- Five anonymous pipes carry fixed-size `Batch` messages downstream.
- All six machines share one atomic telemetry pipe into Quality Control.
- QC forwards `MachineEvent` copies and alarm transitions to the Supervisor.
- Separation sends one `ProductionResult` per completed batch.
- Signals carry only control requests; handlers set `sig_atomic_t` flags.

All pipes are created before `fork()`. Each child closes every descriptor it
does not own. The parent closes all stage and telemetry ends after startup.
Therefore, when Reception closes its output, EOF drains stage by stage until
QC sees telemetry EOF and exits. A forgotten writer would prevent EOF.

## Child ownership

| Child | Batch input | Batch output | Other channels |
| --- | --- | --- | --- |
| Reception | - | Pipe 0 | Telemetry writer |
| Cleaning | Pipe 0 | Pipe 1 | Telemetry writer |
| Washing | Pipe 1 | Pipe 2 | Telemetry writer |
| Crushing | Pipe 2 | Pipe 3 | Telemetry writer |
| Malaxation | Pipe 3 | Pipe 4 | Telemetry writer |
| Separation | Pipe 4 | - | Telemetry + result writers |
| Quality Control | - | - | Telemetry reader, status + alarm writers |

## Control behavior

- `SIGUSR1`: pause while retaining the current batch.
- `SIGUSR2`: resume or clear an injected error.
- Linux real-time signals: inject machine, heat, or pressure faults.
- `SIGTERM`: graceful shutdown; Supervisor escalates after two seconds.

Sensor-limit alarms pause the affected machine and Reception. QC hysteresis
clears the alarm only below the configured recovery threshold; the Supervisor
then resumes the machine and releases Reception. Manual/machine errors use the
ERROR state and require resume/clear.

The GUI timer calls `supervisor_poll(0)`, copies a `FactorySnapshot`, and draws
only that snapshot. No OpenGL callback performs a blocking read. Headless mode
uses the same Supervisor with a bounded polling timeout.

## Reliability rules

- `read_full` and `write_full` retry interrupted and partial transfers.
- Truncated fixed-size messages are protocol errors, not valid batches.
- Production randomness is derived from seed, stream, and batch ID, not process
  scheduling or telemetry frequency.
- Every child PID and exit status is tracked; an abnormal exit stops the line.
- Mass is checked per batch: `original = losses + oil + water + pomace`.
