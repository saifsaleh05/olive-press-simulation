# QC alarm / recovery state-transition table

Owner: Ibrahim. Applies to every production machine separately; limits come
from `config.txt` (`<MACHINE>_MAX_*`, `<MACHINE>_RECOVERY_*`,
`<MACHINE>_MAX_BATCH_TIME`).

QC keeps one flag per *(machine, fault)*: **NORMAL** or **ACTIVE**. It sends
an `Alarm` to the Supervisor **only on a transition**, never while the
condition simply persists, so there is no alarm spam.

| Fault | NORMAL → ACTIVE (`ALARM_RAISED`) | ACTIVE → NORMAL (`ALARM_CLEARED`) | Expected Supervisor reaction (Ameer) |
|---|---|---|---|
| `FAULT_OVERHEAT` | temperature **>** `MAX_TEMPERATURE` | temperature **<** `RECOVERY_TEMPERATURE` | raised: `SIG_PAUSE` the machine, show red. Cleared: `SIG_RESUME`, show green/gray |
| `FAULT_OVERPRESSURE` | pressure **>** `MAX_PRESSURE` | pressure **<** `RECOVERY_PRESSURE` | same as overheat |
| `FAULT_TIMEOUT` | `batch_time` **>** `MAX_BATCH_TIME` (paused or failed time is not counted) | machine reports another batch, or no batch | warn / log; optional pause |
| `FAULT_MACHINE_ERROR` | `state == ST_ERROR` (automatic failure or `SIG_FAULT`) | `state != ST_ERROR` (after `SIG_RESUME`) | raised: red, wait for the operator. Cleared: back to normal |
| any active fault | | machine sends `EV_SHUTDOWN` | final display is consistent |

Hysteresis: a reading between `RECOVERY_*` and `MAX_*` changes nothing. This
keeps a noisy value near the limit from switching between alarm and recovery.
Example (Malaxation, max 35 °C / recovery 30 °C):

| reading | 34 | 35.4 | 36 | 32.5 | 35.2 | 29.9 | 29 |
|---|---|---|---|---|---|---|---|
| QC sends | – | **RAISED** | – | – | – | **CLEARED** | – |

## Fault scenarios and how to trigger them

| Scenario | How | Seen by QC as |
|---|---|---|
| Overheat | continuous work (e.g. Crushing: 24 + 0.8/0.05 = 40 °C > 35 °C after ~23 s), higher `AMBIENT_TEMPERATURE`, or `SIG_HEAT` (+`HEAT_SPIKE` °C) | `FAULT_OVERHEAT` |
| Overpressure | heavy batch (`PRESSURE_GAIN` × kg/25), or `SIG_PRESSURE` (+`PRESSURE_SPIKE` bar) | `FAULT_OVERPRESSURE` |
| Timeout | lower `<M>_MAX_BATCH_TIME`, or raise `<M>_BASE_TIME` | `FAULT_TIMEOUT` |
| Recovery threshold | pause the machine; it cools/depressurises below `RECOVERY_*` | `ALARM_CLEARED` |
| Manual fault | Supervisor sends `SIG_FAULT` (`SIGRTMIN+1`) | `FAULT_MACHINE_ERROR` |
| Automatic fault | `AUTO_FAULTS_ENABLED=1`, each batch fails with `FAULT_PROBABILITY_PERCENT` | `FAULT_MACHINE_ERROR` |

In every case the batch inside the machine is kept. After `SIG_RESUME` it
continues from the same progress.
