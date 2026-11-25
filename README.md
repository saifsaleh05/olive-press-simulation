# Olive Press Simulation

ENCS4330 Project 1: a Linux multiprocessing olive-oil factory simulation using
signals, anonymous pipes, OpenGL/FreeGLUT, and OpenMP.

The parent is the Supervisor and owns the GUI. It forks seven children:
Reception, Cleaning, Washing, Crushing, Malaxation, Separation, and Quality
Control. Five batch pipes form the production line. Six machines send telemetry
to QC; QC forwards status and alarms to the Supervisor; Separation sends final
mass-balanced production results to the Supervisor.

## Linux build

```sh
sudo apt install build-essential freeglut3-dev
make
```

`make` builds the simulator with the OpenGL GUI and the OpenMP benchmark on
Linux. A macOS development build uses a GUI stub because FreeGLUT and GCC
OpenMP are not system components there; `--headless` and the complete test
suite remain available.

## Run

```sh
./olive_press config/default.cfg
./olive_press config/default.cfg --autostart
./olive_press config/default.cfg --headless
```

Configuration files use `KEY=VALUE`; omitted keys keep validated defaults and
unknown keys are rejected. `config.txt` documents every supported setting.

### GUI controls

| Key | Action |
| --- | --- |
| `S` | Start the production line |
| `P` | Pause all machines (`SIGUSR1`) |
| `R` | Resume all machines (`SIGUSR2`) |
| `F` | Inject a fault into the selected machine |
| `C` | Clear the selected fault and resume |
| `X` | Stop and reap every child |
| `1`-`6` | Select a machine |
| `Q` / Esc | Stop and close |

Colors show actual reported state: gray IDLE, green OPERATING, yellow PAUSED,
red ERROR, and blue COMPLETED.

### Headless commands

```text
p       pause
r       resume
f N     fault machine N (1 = Reception, 6 = Separation)
c N     clear machine N
x       stop
```

## Tests

```sh
make test
```

The suite covers IPC short reads/writes and EOF, transactional configuration,
Reception/Cleaning uniqueness and mass accounting, Washing/Crushing,
Malaxation/Separation, QC alarm hysteresis, two-million-batch mass balance,
normal end-to-end production, deterministic output, alarm recovery,
pause/resume/fault/clear, forced stop, crashed-child cleanup, and invalid input.

## OpenMP

```sh
make bench
./omp_bench --sweep
```

The benchmark compares sequential and parallel processing. See
`docs/benchmark.md` for the recorded interpretation.

## Layout

```text
include/       shared Batch, telemetry, alarm, configuration and process APIs
common/        configuration, IPC helpers and shared machine physics
processes/     six real stages, Quality Control and Supervisor
graphics/      OpenGL/FreeGLUT GUI plus a macOS headless stub
benchmark/     OpenMP sequential-versus-parallel benchmark
src/main.c     CLI, signal setup and headless/GUI dispatch
config/        demonstration profiles
tests/         module and end-to-end tests
docs/          design, demo and benchmark notes
```

## Debugging

```sh
make debug
gdb --args ./olive_press tests/fast.cfg --headless
```

Useful GDB commands after `fork()` are `set follow-fork-mode child`,
`set detach-on-fork off`, and `info inferiors`.
