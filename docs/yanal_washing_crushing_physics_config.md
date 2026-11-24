# Washing, Crushing, Physics and Configuration

Owner: Yanal. The shared wire structures remain unchanged from
`include/common.h`; this work adds stage entry points to `include/processes.h`
and reusable physics helpers to `include/simulation.h`.

## Pipeline and messages

```
Cleaning --Batch--> Washing --Batch--> Crushing --Batch--> Malaxation
                         |                 |
                         +--MachineEvent---+--> Quality Control
```

`washing_process` removes a configured fraction of mass, updates the remaining
impurity fraction, marks the batch as washed, emits real state/progress/physics
telemetry, and forwards the same fixed-size `Batch`. `crushing_process`
conserves mass and marks `stage=M_CRUSHING`; under the shared contract this
stage value means that `current_mass` is now paste.

## Deterministic values and ranges

Ranges are bounds for a named physical property, not unrelated values. For
example, `WASHING_LOSS_PERCENT_MIN/MAX` controls only washing loss. The loss is
selected from a fixed seed, the washing stream number, and the batch ID. It is
therefore unchanged if the process happens to emit an extra heartbeat.

Every machine also receives its own deterministic PRNG stream. A fixed
`RANDOM_SEED` and machine ID produce the same sensor sequence, while Washing
and Crushing do not produce identical readings. The project owns the PRNG
algorithm instead of relying on platform-specific `rand_r` behavior.

## Physics model

For a step of `dt` seconds:

```
temperature += (heating_rate - cooling_factor * (temperature - ambient)) * dt
```

`heating_rate` is zero unless the machine is operating. Temperature is stored
in the `Machine`, so it persists between batches and cools toward ambient while
idle, paused, or failed.

The pressure model supplied to QC is:

```
target = base_pressure + pressure_gain * (batch_kg / nominal_batch_kg)
pressure += (target - pressure) * min(dt / pressure_time_constant, 1)
```

When not operating, the target is zero. Ibrahim's QC compares the resulting
sensor reading with each machine's configured maximum and recovery limits.

Processing duration is weight-dependent:

```
duration = base_time * max(batch_kg / nominal_batch_kg, 0.1)
```

## Configuration behavior

`config_load` is transactional. A malformed line, unknown key, non-finite
number, fractional integer, invalid range, or impossible/negative quantity
rejects the entire file and leaves the previous configuration unchanged.

## Tests

- `test_config_simulation`: validation, deterministic streams, heating, idle
  cooling, and weight-dependent processing duration.
- `test_wash_crush`: real Washing and Crushing child processes connected by
  pipes, with the parent acting as mock Malaxation; it verifies order, loss
  bounds, stage transition, telemetry starts/completions, EOF, and clean exits.
