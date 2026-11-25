# ENCS4330 Project #1 - Course Alignment Audit

Audit date: 25 November 2026
Working branch: `audit/course-alignment`
Repository state: local changes only; no commit and no push were made.

## 1. Executive assessment

The project is strongly aligned with the central techniques shown in the supplied course examples:

- all anonymous pipes are created before `fork()`;
- the Supervisor creates seven child processes and records every PID;
- each child closes pipe ends it does not use;
- `Batch`, `MachineEvent`, `Alarm`, and `ProductionResult` messages travel through `read()`/`write()`-based pipes;
- the Supervisor controls children with signals sent by `kill()`;
- children are reaped with `waitpid()`;
- command-line input is read through `argc`/`argv`;
- configuration comes from a text file;
- the process pipeline, Quality Control process, OpenMP comparison, and OpenGL view satisfy the assignment's requested areas.

The architecture is more complete than the small instructor examples, but it is not needlessly replaced. The extra mechanisms are mainly required for a responsive GUI, concurrent stages, safe shutdown, deterministic testing, and robust fixed-size IPC.

Two low-risk alignment changes were made:

1. Production signal registration now uses checked `signal()` calls, the function actually demonstrated in the instructor's signal examples, instead of `sigaction()`.
2. A second configuration filename is now rejected instead of silently replacing the first one, following the explicit argument-validation style in the instructor's argument examples.

No simulation formula, pipeline stage, batch transformation, alarm rule, OpenMP calculation, or graphical behavior was intentionally changed.

## 2. Authoritative material inspected

### Requirements

The complete two-page `project1_signal_pipe_202701.pdf` was extracted and visually inspected page by page. It requires a multiprocessing olive-press simulation, communication using signals and/or pipes and/or FIFOs, configuration loaded from a filename argument, OpenMP evaluation, OpenGL graphics, testing, and debug builds with `-g`. It explicitly says that not every IPC facility must be used.

### Instructor examples

All supplied source/header/build files were read completely. The prebuilt binaries in the archives were inventoried but were not treated as source evidence.

- Basic process creation: `references/basic/fork1.c`, `fork2.c`, `fork3.c`, `fork4.c`
- Program execution: `references/basic/exec1.c`
- Arguments: `references/basic/args.c`, `cos.c`, `references/args/args.c`, `cos.c`, `Makefile`
- Pipes: `references/pipe/pipe1.c`, `pipe2.c`
- Signals: `references/signal/alarm.c`, `kill.c`, `signals1.c`, `signals2.c`, `signals3.c`, `sigpause1.c`
- FIFOs: `references/fifo/client.c`, `server.c`, `example.c`, `local.h`, `local1.h`, `compile_d_1.c`, `compile_d_2.c`

Important evidence boundaries:

- The examples demonstrate `fork`, `getpid`, `wait`, `waitpid`, `pipe`, `read`, `write`, `close`, `kill`, `signal`, `sigset`, `pause`, `alarm`, `sleep`, `open`, `mknod`, `unlink`, `argc`/`argv`, `atof`, `printf`, `fprintf`, `sprintf`, `perror`, `fflush`, `strlen`, `strncmp`, and `memset`.
- They do **not** demonstrate `sigaction`, `poll`, `clock_gettime`, `nanosleep`, `sysconf`, OpenMP, OpenGL, the configuration parser, or the project's robust full-message wrappers.
- Some examples are deliberately simple or incomplete. Examples include infinite loops, signal-handler printing, a misspelled executable in `exec1.c`, and malformed/conditional FIFO snippets. Those defects were not copied into working project code.

## 3. Project files inspected

Every project C source and header was read completely:

- Build/entry: `Makefile`, `src/main.c`
- Public headers: `include/common.h`, `include/config.h`, `include/ipc.h`, `include/processes.h`, `include/simulation.h`
- Common code: `common/config.c`, `common/ipc.c`, `common/names.c`, `common/simulation.c`
- Processes: `processes/reception.c`, `cleaning.c`, `washing.c`, `crushing.c`, `malaxation.c`, `separation.c`, `quality_control.c`, `supervisor.c`, `supervisor.h`
- Graphics: `graphics/graphics.c`, `graphics/graphics.h`, `graphics/graphics_stub.c`
- OpenMP: `benchmark/openmp_benchmark.c`
- Tests: all seven `tests/test_*.c` files and `tests/run_headless.sh`
- Configuration: `config/default.cfg`, `config/hot_day.cfg`, `tests/fast.cfg`, `tests/fast_hot.cfg`

Supporting README and team documentation are secondary; the source code, instructor examples, and requirements were treated as authoritative for this audit.

## 4. Architecture and execution flow

1. `main` parses the optional configuration path and run-mode options, loads and validates configuration, initializes the Supervisor, and enters either the headless loop or GLUT loop.
2. `supervisor_start` creates five stage-to-stage pipes plus telemetry, alarm, result, and forwarded-status pipes before any fork.
3. The Supervisor forks Reception, Cleaning, Washing, Crushing, Malaxation, Separation, and Quality Control.
4. Each child closes unrelated descriptors. Production stages retain their input/output and shared telemetry write end; QC retains telemetry input plus alarm/status outputs.
5. Reception deterministically creates batches. Each stage processes one batch while other stages can process other batches concurrently.
6. Cleaning and Washing subtract configured losses. Crushing conserves mass. Malaxation may reduce quality when overheated. Separation creates oil, vegetable water, pomace, and pre-separation losses with an exact mass balance.
7. All six machines emit telemetry. QC evaluates temperature, pressure, batch duration, and error state, then sends alarm transitions to the Supervisor.
8. The Supervisor multiplexes telemetry, alarms, and results, sends control signals, updates the GUI snapshot, reaps children, and escalates from `SIGTERM` to `SIGKILL` only after a grace period.
9. EOF propagates downstream as writers close pipe ends. The Supervisor finishes only after all children are reaped and pending uplink messages are drained.

## 5. Alignment matrix for core course topics

| Topic | Project implementation | Exact instructor evidence | Assessment |
|---|---|---|---|
| Process creation | `pipe()` then seven `fork()` calls | `basic/fork1.c:14`, `fork3.c:18`; `pipe/pipe1.c:20,25`; `pipe2.c:15,22` | Directly aligned |
| Parent/child distinction | `pid < 0`, `pid == 0`, parent PID storage | `basic/fork3.c:20-33`; `pipe/pipe1.c:25-43` | Directly aligned |
| Waiting/reaping | blocking `waitpid` during failure cleanup; nonblocking `waitpid(..., WNOHANG)` during GUI operation | `basic/fork3.c:37`; `signal/kill.c:44`; `basic/fork4.c:30` | Function covered; nonblocking option is an application extension |
| Signals | `signal()` registration; `kill()` delivery; handlers set flags | `signal/signals2.c:24,50`; `signals3.c:21,41`; `kill.c:39` | Function usage aligned; flag-only handlers are safer than example printing |
| Anonymous pipes | `pipe`, close unused ends, `read`, `write` | `pipe/pipe1.c:20-45`; `pipe2.c:15-56` | Directly aligned |
| FIFO | Not used | `fifo/client.c`, `fifo/server.c` | Allowed: PDF says signals/pipes/FIFOs are alternatives and all are not required |
| Arguments | manual `argc`/`argv` parsing and validation | `args/cos.c:10-29`; `basic/exec1.c:8-15` | Aligned |
| Termination | child `_exit`; Supervisor `SIGTERM`, timed `SIGKILL`, `waitpid` | examples use `exit`, `kill`, `waitpid` | Safer child-exit variant retained and documented |
| Debug build | default build includes `-g`; `make debug` uses `-O0 -g` | `references/args/Makefile:5-7` | Directly aligned |

## 6. Refactoring performed

| Change | Original implementation | Updated implementation | Instructor reference | Why | Behavior effect |
|---|---|---|---|---|---|
| Parent signal registration | `sigaction()` for `SIGINT`/`SIGTERM` | checked `signal()` calls in `src/main.c:32-43` | `references/signal/signals2.c:24,50`; `signals3.c:21,41` | Uses the exact taught registration function and reports registration failure | No intended simulation change |
| Machine signal registration | helper built a `struct sigaction` with `SA_RESTART` | helper uses checked `signal()` in `common/simulation.c:33-49` | same files as above | Removes an untaught registration API; all blocking code already handles interruption or loops | No intended pipeline or signal-protocol change |
| QC signal registration | `sigaction()` for stop signals | checked `signal()` in `processes/quality_control.c:136-148` | same files as above | Aligns QC with taught technique | No intended behavior change |
| Supervisor `SIGPIPE` ignore | `sigaction(SIGPIPE, SIG_IGN)` | checked `signal(SIGPIPE, SIG_IGN)` in `processes/supervisor.c:97-117` | `references/signal/signals2.c:24`; `signals3.c:21` | Uses taught ignore pattern and propagates failure | Only affects the exceptional case where registration fails |
| Command-line ambiguity | every positional argument overwrote `cfg_path`; the last file silently won | a second config path is rejected at `src/main.c:124-144` | `references/args/cos.c:14-27` | Makes argument validation explicit and predictable | Invalid two-file invocations now fail; valid runs unchanged |
| Initialization error propagation | `supervisor_init()` return value was ignored and parent registration errors were only logged | both are checked at `src/main.c:152-155` | error-checking style in `pipe/pipe1.c:20-28` and `basic/fork1.c:14-16` | Prevents startup after failed signal setup | Only changes an initialization-failure path |

`sigaction()` remains in `tests/test_ipc.c:80-85` on purpose. That test must install a handler **without** automatic restart so it can prove that `read_full()` correctly retries an `EINTR`; replacing it with `signal()` on Linux could invalidate the test.

## 7. External APIs directly demonstrated by the instructor

These project APIs need no "outside material" label because the exact function, not merely its library, appears in the supplied examples.

| Exact function/technique | Project use | Instructor evidence |
|---|---|---|
| `fork()` | create production/test children; returns -1, 0, or child PID | `basic/fork1.c:14`, `fork3.c:18`; `pipe/pipe1.c:25`, `pipe2.c:22` |
| `getpid()` | telemetry/log PID identity; returns caller PID | `basic/fork1.c:11`; `signal/kill.c:17` |
| `wait()` / `waitpid()` | reap children and collect status; return child PID/-1 | `basic/fork4.c:30`; `basic/fork3.c:37`; `signal/kill.c:44` |
| `WEXITSTATUS(status)` | extract normal child exit code | `signal/kill.c:45` |
| `pipe(fd)` | create anonymous read/write descriptor pair; returns 0/-1 | `pipe/pipe1.c:20`; `pipe2.c:15` |
| `read(fd,buf,n)` / `write(fd,buf,n)` | underlying project IPC; return byte count, 0 EOF for read, or -1 | `pipe/pipe1.c:32,45`; `pipe2.c:39,54`; FIFO client/server |
| `close(fd)` | release unused/finished pipe ends; returns 0/-1 | `pipe/pipe1.c:31,43`; `pipe2.c:32,40,49,56` |
| `kill(pid,signal)` | Supervisor control and test fault injection; returns 0/-1 | `signal/kill.c:39` |
| `signal(number,handler)` | register/ignore handlers; returns previous handler or `SIG_ERR` | `signal/signals2.c:24,50`; `signals3.c:21,41` |
| `exit(status)` | benchmark argument/help failure only; does full C cleanup and does not return | `args/cos.c:16,21,26`; process examples |
| `printf`, `fprintf`, `perror`, `fflush` | normal output, diagnostics, and buffer control | `pipe/pipe1.c:16,21,33-50`; many other examples |
| `memset` | zero structures/buffers | `pipe/pipe2.c:52`; FIFO examples |
| `strlen` / `strncmp` | string length and bounded comparison | `pipe/pipe1.c:45`; FIFO client `:34` |
| `unlink(path)` | test temporary-file cleanup; returns 0/-1 | `fifo/client.c:54` |
| `argc` / `argv` | application and benchmark option parsing | all argument examples, especially `args/cos.c:10-29` |

## 8. Remaining functions and techniques outside the supplied examples

Project-owned functions such as `machine_run_stage`, `write_full`, `qc_evaluate`, and `separation_compute` are **not external library functions**. They are discussed where their techniques matter, but they are not mislabeled as unfamiliar APIs.

### 8.1 Multiprocessing and process management

| Function or technique | Source file and line | Purpose / arguments / return | Why it is needed | Why it was not replaced | Possible course alternative |
|---|---|---|---|---|---|
| `_exit(status)` | `processes/supervisor.c:218`; several process tests | Immediately terminates the current process with the supplied status; never returns and does not flush inherited stdio again | Correct exit immediately after `fork()` | `exit()` could double-flush inherited buffers and run parent cleanup | Instructor uses `exit()`, but `_exit()` is safer here |
| `waitpid(..., WNOHANG)` | `processes/supervisor.c:455` | Checks for any exited child without blocking; returns PID, 0 if none, or -1 | GUI/headless Supervisor must remain responsive | Blocking `waitpid(...,0)` would freeze event processing | Blocking `waitpid` from `basic/fork3.c:37` only during final cleanup |
| `WIFEXITED`, `WIFSIGNALED`, `WTERMSIG` | `processes/supervisor.c:463-476`; tests | Macros inspect the status written by `waitpid`; return boolean/status information | Distinguishes success, error exit, and signal death | Raw status integers are not reliably readable | Example only shows `WEXITSTATUS` in `signal/kill.c:45` |
| `sysconf(_SC_OPEN_MAX)` | `processes/supervisor.c:131` | Queries the process descriptor limit; returns a long or -1 | Child must close inherited GUI and unrelated descriptors, not only known pipe arrays | Explicitly closing project pipes would leave inherited GLUT/X11 descriptors; `closefrom` is less portable | Manual `close()` calls as in `pipe2.c`, insufficient for unknown GUI descriptors |
| `getppid()` | `tests/test_ipc.c:92` | Takes no arguments and returns parent PID | EINTR test sends an alarm to its test parent | Test-only and direct | Pass the parent PID through test state |

### 8.2 Signals

| Function or technique | Source file and line | Purpose / arguments / return | Why it is needed | Why it was not replaced | Possible course alternative |
|---|---|---|---|---|---|
| `volatile sig_atomic_t` flags | `common/simulation.c:20-24`; `processes/quality_control.c:128`; `supervisor.c:35-36` | Integer type that can be read/written atomically by a signal handler | Keeps handlers async-signal-safe; real work happens in normal control flow | Instructor handlers call `printf`/`exit`, which is unsafe in a robust program | Plain global `int` as in `signal/alarm.c:13`, but not recommended |
| Real-time signals `SIGRTMIN+1..3` | `include/ipc.h:44-47` | Distinct queued Linux signal numbers for fault, heat, and pressure injection | Avoids overloading pause/resume and keeps commands distinct | Standard signals alone do not provide enough independent user-defined controls | Reuse fewer standard signals plus an additional command pipe |
| `SIGPIPE` ignored | `common/simulation.c:49`; `quality_control.c:147`; `supervisor.c:113` | Converts a broken pipe from process termination into `write()` returning `EPIPE` | Enables controlled cleanup when a reader disappears | Default termination would bypass cleanup | No equivalent shown; instructor examples assume live readers |
| `sigaction()` in EINTR test only | `tests/test_ipc.c:80-85` | Installs `SIGALRM` handler with no `SA_RESTART`; returns 0/-1 | Forces blocked `read()` to return `EINTR` | `signal()` may set automatic restart on Linux | No reliable course-only substitute for this specific test |

### 8.3 Pipes, FIFOs, and other IPC

| Function or technique | Source file and line | Purpose / arguments / return | Why it is needed | Why it was not replaced | Possible course alternative |
|---|---|---|---|---|---|
| `poll(fds,n,timeout)` | `src/main.c:87`; `common/simulation.c:323`; `quality_control.c:192`; `supervisor.c:531`; integration test | Waits for readiness on one or more descriptors; returns ready count, 0 timeout, -1 error | Supervisor must watch three uplinks and GUI/stdin without blocking; stages must remain signal-responsive while idle | Sequential blocking `read()` would freeze other channels or UI work | Separate blocking reader processes, a major architecture change |
| Full-message retry loop | `common/ipc.c:7-45` | Team-owned wrappers repeat `read`/`write` after short transfers or `EINTR` | Prevents truncated structs and handles signals | Instructor examples issue one call and omit these production edge cases | Direct `read`/`write` as in `pipe1.c`, less robust |
| Fixed-size raw-struct protocol | `include/common.h:50-129`; `include/ipc.h:6-19` | Sends identical in-memory structures between forked processes | Simple, deterministic, and under `PIPE_BUF` | Text serialization would add parsing and more failure cases | Character messages as in `pipe1.c` |
| Shared atomic telemetry writers | `include/ipc.h:13-15`; `processes/supervisor.c:145-181` | Six children share one pipe write end; each event is smaller than `PIPE_BUF` | Avoids six separate Supervisor channels | Safe because each event is one atomic blocking write | One pipe per child as in `pipe2.c`; more descriptors and polling entries |
| FIFO omitted | whole project | No named pipe is created | Anonymous pipes fit related forked processes and clean up automatically | PDF explicitly says all mechanisms are not required | `mknod/open/unlink` pattern from FIFO examples |

### 8.4 Configuration and file handling

| Function or technique | Source file and line | Purpose / arguments / return | Why it is needed | Why it was not replaced | Possible course alternative |
|---|---|---|---|---|---|
| `fopen(path,"r")` / `fclose(FILE*)` | `common/config.c:202,245` | Opens a configuration stream (`NULL` on failure) and closes it (0/-1) | Assignment requires values loaded from a text file | Low-level `open/read` would complicate line parsing | `open/read` is shown in FIFO examples, but is less suitable here |
| `fgets(buf,size,stream)` | `common/config.c:209`; `src/main.c:89` | Reads at most `size-1` characters and returns buffer/`NULL` | Safe bounded line input for config and commands | Instructor examples use raw `read`, which does not provide lines/termination | `read()` plus manual buffering |
| `fgetc`, `feof`, `ferror` | `common/config.c:213-242` | Read one character; test EOF; test stream error | Detects oversized lines and real I/O failures | Required for strict validation | Raw `read()` with manual state |
| `strtod(text,&end)` | `common/config.c:78` | Parses a double and reports where parsing stopped; returns the value and sets `errno` | Rejects partial, overflowed, and invalid values | `atof()` in the example cannot report errors | `atof()` from `args/cos.c`, but unsafe for validation |
| `offsetof(type,member)` | `common/config.c:95,126` | Compile-time byte offset of a structure member | Drives a compact table-based parser without one branch per key | Replacing it would duplicate dozens of assignments | Long `strcmp`/assignment chain |
| `mkstemp`, `fdopen`, `fputs` | `tests/test_config_simulation.c:25,31,37` | Creates a unique temp file (returns fd), wraps it as `FILE*`, and writes a string (nonnegative/EOF result) | Safely tests invalid config files | Test-only; fixed temp names would race | A fixed file created with `open`, less safe |

### 8.5 OpenMP

The instructor archives contain no OpenMP example. OpenMP is nevertheless explicitly required by the assignment PDF.

| Function or technique | Source file and line | Purpose / arguments / return | Why it is needed | Why it was not replaced | Possible course alternative |
|---|---|---|---|---|---|
| `#pragma omp parallel for` | `benchmark/openmp_benchmark.c:119`; `tests/test_separation_mass.c:216` | Splits loop iterations across threads; clauses select thread count, static schedule, and reductions | Implements and measures required loop parallelism | Required by assignment | Sequential loop retained for comparison |
| `reduction` | same lines | Creates per-thread accumulators and combines them safely | Avoids data races in totals/maxima | Manual locks are slower and more complex | Run sequentially |
| `omp_get_wtime()` | benchmark `:104,111,116,125`; mass test `:202-226` | Takes no arguments; returns wall-clock seconds as `double` | Compares sequential and parallel execution | Standard C clock is less appropriate for elapsed parallel timing | `clock_gettime`, also outside examples |
| `omp_get_max_threads()` | benchmark `:152`; mass test `:241` | Returns default maximum team size | Selects/reports available threads | Needed for meaningful comparison | User-supplied fixed count |
| `omp_get_num_procs()` | benchmark `:203` | Returns processors visible to OpenMP | Reports test environment | Informational and useful for discussion | Omit the information |

### 8.6 OpenGL / FreeGLUT

The instructor archives contain no OpenGL example. These APIs remain because OpenGL graphics are explicitly required.

| Function or technique | Source file and line | Purpose / arguments / return | Why it is needed | Why it was not replaced | Possible course alternative |
|---|---|---|---|---|---|
| `glBegin(mode)` / `glEnd()` | `graphics/graphics.c:99-127,185-195,402-405` | Delimit immediate-mode vertex submission; no return value | Draws simple rectangles, lines, circles, and arrows | Simple and appropriate for the assignment's basic graphics | None in supplied material |
| `glVertex2f(x,y)` | `graphics/graphics.c:100-125,186-194,403-404` | Adds one 2-D vertex; no return | Defines geometry | Required by immediate mode | None |
| `glColor3f(r,g,b)` | `graphics/graphics.c:89` | Sets current RGB color; no return | State/fault coloring | Required for visualization | None |
| `glLineWidth(width)` | `graphics/graphics.c:109,116,184,189` | Sets line width; no return | Borders and arrows | Visual clarity | None |
| `glRasterPos2f(x,y)` | `graphics/graphics.c:132` | Sets bitmap-text origin; no return | Labels/status text | Required by GLUT bitmap fonts | None |
| `glClearColor` / `glClear` | `graphics/graphics.c:499-500` | Set clear color; clear selected buffers; no return | Redraws every frame | Standard frame setup | None |
| `glViewport`, `glMatrixMode`, `glLoadIdentity` | `graphics/graphics.c:590-595` | Configure viewport and active matrix, then reset it; no return | Resizes the logical canvas correctly | Needed for window resizing | None |
| `glEnable`, `glBlendFunc` | `graphics/graphics.c:625-627` | Enable capabilities and select blend factors; no return | Smooth lines and alpha blending | Visual quality | None |
| `gluOrtho2D(left,right,bottom,top)` | `graphics/graphics.c:593` | Defines a 2-D orthographic projection; no return | Maps the fixed logical canvas to the window | Simplest 2-D projection | None |
| `glutBitmapCharacter`, `glutBitmapString`, `glutBitmapLength` | `graphics/graphics.c:135,137,143` | Draw one character/draw a string/measure a string; drawing returns void and length returns pixels | Portable text on native macOS GLUT and Linux FreeGLUT | No built-in core OpenGL text | Character loop is the macOS-compatible implementation |
| Window setup: `glutInit`, `glutSetOption`, `glutInitDisplayMode`, `glutInitWindowSize`, `glutCreateWindow` | `graphics/graphics.c:617-623` | Initialize GLUT and create the window; create returns a window ID, others are setup calls | Required to open the assignment GUI | No course alternative | None |
| Callback registration: `glutDisplayFunc`, `glutReshapeFunc`, `glutKeyboardFunc`, `glutMouseFunc`, `glutTimerFunc` | `graphics/graphics.c:610,629-633` | Register function pointers for display/input/timer events; no useful return | Event-driven responsive UI | Polling a window manually is not the GLUT model | None |
| Loop/update: `glutMainLoop`, `glutLeaveMainLoop`, `glutPostRedisplay`, `glutSwapBuffers` | `graphics/graphics.c:511,522,540,550,580,609,639` | Enter/leave event loop, request redraw, present back buffer | Runs and updates the GUI without flicker | Required GLUT lifecycle | None |

### 8.7 Standard C library functions not present in the examples

| Function or technique | Source file and line | Purpose / arguments / return | Why it is needed | Why it was not replaced | Possible course alternative |
|---|---|---|---|---|---|
| `snprintf` / `vsnprintf` | config `:267`; simulation `:73`; supervisor `:68-70`; graphics `:155,214,422-474` | Bounded formatted output; return required character count; `vsnprintf` consumes a `va_list` | Prevents buffer overflow in logs/UI/errors | `sprintf` in FIFO example is unbounded | `sprintf`, not recommended |
| `sscanf(text,format,...)` | `src/main.c:50` | Parses formatted input; returns assignments completed | Parses headless machine number | Manual character parsing is longer | `atof`-style conversion, but command contains a letter prefix |
| `strcmp`, `strchr`, `strstr` | `src/main.c:125-129`; `common/config.c:145-227`; graphics `:481` | Compare strings; find a character; find a substring; return ordering/pointer/`NULL` | Option parsing, config tokenization, log coloring | Direct and standard | `strncmp` exists in FIFO example but is not always equivalent |
| `strerror(error)` | simulation `:36,174`; supervisor `:93,212,437,535` | Returns readable text for an error number | Detailed error messages | `perror` cannot be embedded inside structured log lines | Use separate `perror()` calls |
| `memcpy` / `memcmp` | config `:53`; graphics `:158`; tests and benchmark comparisons | Copy bytes (returns destination); compare bytes (negative/zero/positive) | Copies fixed tables/strings and verifies deterministic results | Field-by-field code is verbose | Loops over fields/elements |
| `setvbuf(stream,buf,mode,size)` | all stage processes and QC; integration tests | Selects line buffering; returns 0/nonzero | Keeps logs timely without manual flush after every line | Harmless standard stream configuration | Call `fflush` after every line |
| `isspace`, `isfinite`, `floor` | `common/config.c:64-81,162-172,261-319` | Character classification; finite-number test; round down | Strict safe configuration validation | `atof` plus unchecked casts would accept bad input | Simplified validation from `args/cos.c`, less safe |
| `fabs`, `fabsf`, `fmin`, `fmax`, `cosf`, `sinf`, `sqrtf`, `exp` | supervisor `:408`; graphics `:125,179,270,279`; benchmark `:59,66`; tests | Absolute value, bounds, trigonometry, square root, exponential; return numeric result | Mass tolerance, geometry, and benchmark physics | Assignment features require the calculations | `cos`/`sin` examples cover related double variants only |
| `malloc(size)` / `free(ptr)` | benchmark `:190-243`; separation mass test `:171-251` | Allocate heap memory (`NULL` on failure) and release it | Data sets are too large for stack allocation | Static maximum arrays would waste memory or impose limits | Small fixed arrays |
| `qsort(base,n,size,cmp)` | benchmark `:136` | In-place generic sort; no result | Computes median timing robustly | Hand-written sort adds code and risk | Report a single run or arithmetic mean |
| `atoi`, `strtoul` | benchmark `:165-169`; mass test `:149` | Convert string to int/unsigned long; return converted value | Benchmark-only numeric options | `atof` is the taught floating converter, not an exact integer replacement | Manual validation plus `atof`, less appropriate |
| `rand_r(seed)` | `tests/test_separation_mass.c:48,184` | Reentrant pseudo-random integer using caller-owned state | Test-only deterministic bulk data | Project production uses its own deterministic generator; changing test is unnecessary | `rand()` with global state, unsafe for parallel extension |
| `va_start` / `va_end` | supervisor `:69-71`; graphics `:154-156` | Initialize/finalize traversal of variadic arguments | Reusable formatted log/text helpers | Repeating fixed-argument functions would be impractical | Direct `printf` calls everywhere |

### 8.8 Other advanced techniques

| Function or technique | Source file and line | Purpose | Why it is needed | Why it was not replaced | Possible course alternative |
|---|---|---|---|---|---|
| `clock_gettime(CLOCK_MONOTONIC,...)` | simulation `:61`; supervisor `:57,105`; fallback mass test `:25` | Monotonic elapsed time, unaffected by wall-clock changes; returns 0/-1 | Physics steps, deadlines, and timings require elapsed time | `time()` is coarse and wall-clock based | Tick counters, less accurate under scheduling delays |
| `gettimeofday`, `localtime_r` | `common/simulation.c:71-73` | Wall-clock timestamp and thread-safe/local reentrant conversion | Human-readable log prefix | Pure elapsed time is less friendly in logs | Omit wall-clock prefix |
| `nanosleep(request,remain)` | `common/simulation.c:82`; `tests/test_ipc.c:25` | High-resolution interruptible sleep; returns 0/-1 | Millisecond simulation ticks and tests | `sleep()` only accepts seconds | `sleep()` from examples, too coarse |
| Function pointers/callbacks | `include/simulation.h:55-59`; graphics callback setup `:629-633` | Pass stage-specific work into shared runtime and register GLUT events | Avoids duplicating the machine loop and is required by GLUT | Replacing would duplicate large loops/switches | Separate copied loop per process |
| Variadic logging | `processes/supervisor.c:62-75`; graphics `:146-158` | Accepts printf-like variable arguments | Central bounded logging/text | Direct repeated formatting would be harder to maintain | Multiple fixed helpers |
| GCC attributes `format`, `noinline` | `processes/supervisor.h:78`; graphics `:137`; benchmark `:73` | Compile-time format checking; prevent benchmark optimizer from folding work | Warning quality and benchmark validity | Removing attributes loses checks/measurement integrity | None in supplied material |
| Designated initializers / compound literals | many structures, e.g. config `:17-22,27-32`; graphics `:55-69` | C99 named initialization and temporary structures | Makes enum-indexed configuration/state tables auditable | Positional initialization is more error-prone | Assign fields one at a time |
| Deterministic custom PRNGs | `common/simulation.c:85-110`; benchmark `:43-55` | Project-owned xorshift/hash and SplitMix transforms | Results do not depend on scheduling or libc implementation | Not external APIs; retain for repeatability | `rand()`/`rand_r()` with scheduling coupling |
| RK4 numerical integration | `benchmark/openmp_benchmark.c:63-99` | CPU-heavy independent sample calculation | Gives OpenMP enough legitimate work for useful comparison | A trivial loop may be optimized away or show only overhead | Parallelize the lighter separation bulk test only |
| Native macOS GLUT / Linux FreeGLUT build | `Makefile:13-25`; `graphics/graphics.c:1-5,129-139,516-524,615-620` | Builds the same GUI with the platform's available GLUT API; macOS draws strings character-by-character and exits through project cleanup | Allows local visual verification without X11 or new dependencies while preserving the Linux target | A stub prevented the required GUI from being tested locally | Keep a stub only for environments with no GLUT at all |

## 9. Functions intentionally retained despite course mismatch

- `poll` is necessary for a responsive Supervisor and GUI while monitoring multiple pipe inputs.
- `waitpid(..., WNOHANG)` prevents the GUI/event loop from blocking while children are alive.
- `_exit` is the correct post-fork termination primitive and avoids duplicated buffered output.
- `sysconf` plus descriptor sweeping closes inherited GLUT/X11 descriptors as well as project pipes.
- robust full-message loops prevent signal interruptions and truncated structures.
- `sigaction` remains only in the EINTR unit test, where its exact restart semantics are the subject of the test.
- all OpenMP and OpenGL APIs remain because the assignment explicitly requires those technologies.
- strict `strtod`-based configuration parsing remains because replacing it with `atof` would silently accept malformed input.

## 10. Build and test results

### Normal warning-enabled build

- Clean build completed with Apple Clang 16 using `-std=gnu11 -Wall -Wextra -Wshadow -Wformat=2 -Wpedantic -g`.
- No compiler warnings or errors were emitted.
- All seven C test executables passed.
- All seven end-to-end checks in `tests/run_headless.sh` passed.
- Two simultaneous configuration filenames are now rejected with exit status 1.

### Behavior verified

- Reception -> Cleaning -> Washing -> Crushing -> Malaxation -> Separation flow
- Multiple batches concurrently occupying different production stages
- Deterministic batch generation and repeatable output for a fixed seed
- Cleaning/washing debris removal and final per-batch/global mass balance
- Telemetry forwarding and fixed-size IPC integrity
- Pause, resume, manual failure, repair, heat alarm, and automatic recovery
- Quality Control hysteresis and no duplicate alarm spam
- Graceful operator stop
- Emergency reaction to a child killed with `SIGKILL`
- EOF propagation and normal process termination
- Missing/invalid configuration rejection and transactional config behavior
- Real OpenGL interface rendering, live machine state/progress, production totals, event log, and completed 7/7-batch display on native macOS GLUT

### Resource/process audit

A live `lsof` check showed:

- Supervisor: standard descriptors plus exactly three uplink read descriptors.
- Reception: output + telemetry only.
- Intermediate stages: input + output + telemetry only.
- Separation: input + result + telemetry only.
- QC: telemetry input + alarm/status outputs only.

After a complete run, `pgrep` found no matching process. Tests also wait/reap every child. No zombie or stray simulator process was observed.

### Sanitizers

- UndefinedBehaviorSanitizer: the complete test suite passed with `halt_on_error=1`; no finding occurred.
- AddressSanitizer: the program compiled, but Apple's sanitizer runtime aborted during its own initialization. Leak detection explicitly reports that it is unsupported on this platform, so no valid ASan result can be claimed.

### Not testable in this environment

- Linux FreeGLUT specifically: the real interface was built and visually verified with native macOS GLUT, but the Linux FreeGLUT linkage still requires the target lab machine.
- Real multi-thread OpenMP: Apple Clang here has no configured OpenMP runtime/header, so the Linux OpenMP benchmark could not be compiled or run. The non-OpenMP fallback mass test ran with one thread only.
- Post-refactor Linux CI: intentionally not run because it would require pushing changes, which the user prohibited. No claim is made that the current uncommitted revision has passed remote CI.

## 11. Unresolved issues

1. **`QUEUE_CAPACITY` has no runtime effect.** It is parsed (`common/config.c:167-177`), validated (`:291`), and printed (`:347-352`), but no pipe is resized and no user-space bounded queue enforces the value. The kernel's pipe capacity supplies back-pressure instead. A correct fix would require Linux-specific `fcntl(F_SETPIPE_SZ)` with minimum-page caveats or a larger queue/acknowledgement architecture. That is outside the instructor examples and would risk changing pipeline behavior, so it was documented rather than changed.
2. **Linux FreeGLUT and OpenMP require final target validation.** The same GUI source ran successfully with native macOS GLUT, but the exact Linux libraries and multi-thread benchmark must be checked on the lab machine before submission.
3. **Raw structures are an internal ABI protocol.** This is safe because all forked processes execute one binary, but the protocol is not suitable for unrelated executables built with different compilers/ABIs.

## 12. Study list for the project discussion

Every team member should be able to explain:

1. Why all pipes are created before `fork`, and why every unused end must be closed.
2. The exact descriptors retained by each of the seven children.
3. How EOF moves from Reception through Separation to QC/Supervisor.
4. Why messages below `PIPE_BUF` can safely share the telemetry pipe.
5. Why signal handlers only modify `volatile sig_atomic_t` flags.
6. Why `SIGPIPE` is ignored and `EPIPE` is handled.
7. Difference between `exit` and `_exit` after `fork`.
8. Difference between blocking `waitpid` and `WNOHANG`, and how zombies are prevented.
9. Why `poll` is needed for multiple uplinks and a responsive GUI.
10. How `Batch.current_mass`, `impurity_fraction`, quality, and final losses change stage by stage.
11. The two mass-balance equations used by Separation.
12. QC threshold/hysteresis logic and alarm raise/clear transitions.
13. Transactional configuration parsing and why `strtod` is safer than `atof`.
14. OpenMP `parallel for`, static scheduling, and reductions; why parallel execution can be faster or slower.
15. GLUT's callback/event loop and how it reads a Supervisor snapshot without doing process work inside drawing functions.
16. The unresolved `QUEUE_CAPACITY` limitation.

## 13. Files changed by this audit

- `src/main.c`
- `Makefile`
- `common/simulation.c`
- `graphics/graphics.c`
- `processes/quality_control.c`
- `processes/supervisor.c`
- `COURSE_ALIGNMENT_REPORT.md` (new)

The supplied instructor archives were expanded under `references/` because that directory was absent from the clean checkout. Their contents were not edited. Build products and temporary PDF renders are not source changes and must not be committed.
