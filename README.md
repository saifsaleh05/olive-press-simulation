# Olive Press Simulation

This branch contains Saif's initial local implementation for process lifecycle,
Reception, Cleaning, and reliable fixed-size pipe transport. Washing, Crushing,
Malaxation, Separation, and Quality Control are intentionally marked as
placeholders until the teammates who own them integrate their modules.

## Build and test

```sh
make
make test
```

The current scaffold uses validated default configuration values. The team's
configuration-file parser can replace `config_set_defaults` without changing
the Reception or Cleaning interfaces. Reception and Cleaning durations are
already configuration fields; unit tests set them to zero, while the full
simulation uses short visible delays.

## Batch pipe ownership

| Pipe | Reader | Writer |
| --- | --- | --- |
| 0 | Cleaning | Reception |
| 1 | Washing | Cleaning |
| 2 | Crushing | Washing |
| 3 | Malaxation | Crushing |
| 4 | Separation | Malaxation |

After `fork`, every child closes every descriptor except the one or two ends in
its row. The Supervisor closes all ten stage-pipe descriptors after all children
are created. This is necessary for EOF: a reader sees EOF only after every copy
of that pipe's write end has been closed.

## Current message contract

`Batch` is a fixed-size structure containing a unique ID, original/current
mass, olive and impurity mass, cumulative removed impurity, oil potential,
quality, and current stage. `read_full` and `write_full` prevent short system
calls from turning one structure into a truncated message.

## Reception and Cleaning termination

Reception generates deterministic batches until the configured container mass
is exhausted, then returns and closes its output descriptor. Cleaning continues
to read and process every batch already queued in the pipe. Only after it reads
EOF does it return and close its own output, allowing EOF to propagate to
Washing. A stopped conveyor therefore means "no new batches" rather than
"discard batches already in the pipeline."
