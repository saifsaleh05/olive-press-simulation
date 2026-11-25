# Demo script

About 8 minutes. Build first with `make clean && make`, and make sure
`make test` passes on the demo machine.

## 1. Normal run (2 min)

```sh
./olive_press config/default.cfg
```

1. Window opens, all six boxes gray (IDLE), status READY. Nothing has been
   forked yet: `ps --forest -C olive_press` in a second terminal shows one
   process.
2. Click **Start**. Show `ps --forest` again: one parent, seven children, same
   PIDs as on the boxes.
3. Point at:
   * green boxes and the olive circles moving through them,
   * circles on the arrows: batches sitting in a pipe, waiting for a slower
     stage (malaxation is the bottleneck),
   * temperatures rising while a machine works and falling while it is idle,
   * Production panel filling in, and "mass balance err" staying at 0.
4. Let reception finish: it turns blue (COMPLETED) and its PID shows
   "(exited)". The EOF then travels down the line.

## 2. Operator controls (2 min)

On a running line:

1. **Pause**: all six turn yellow, temperatures start dropping. Mention this
   is `SIGUSR1` to six PIDs.
2. **Resume**: back to green, nothing lost.
3. Click **Crushing**, then **Inject fault**: crushing goes red, reception goes
   yellow (held so no new olives enter), and the batches already past
   reception keep moving until they queue up in front of crushing.
4. **Clear fault**: crushing continues, reception is released.
5. **Stop**: everything stops within a moment; terminal shows each child
   exiting. `ps` shows no leftover processes.

## 3. Automatic fault handling (2 min)

```sh
./olive_press config/hot_day.cfg --autostart
```

Ambient 24 C and a hard working malaxer. After about 12 s malaxation passes the
27 C cold extraction limit:

* QC sends an Alarm, the supervisor faults the malaxer (red) and holds
  reception (yellow),
* the malaxer cools; once it is below 27 x 0.93 the supervisor clears it
  automatically and the line carries on.

The event log shows the alarm and the clear in red.

## 4. Crash handling (30 s)

With a line running:

```sh
kill -9 <pid of washing from the GUI>
```

Washing goes red with "process died", the supervisor stops the rest safely,
no zombies (`ps` shows nothing).

## 5. Benchmark (1 min)

```sh
./omp_bench --sweep
./omp_bench -n 2 -s 10 -r 101 --sweep
```

First: speedup close to the number of cores, results match. Second: OpenMP is
slower on a tiny job because starting the thread team costs more than the work.

## Questions to be ready for

* **Why does the GUI run in the parent?** The supervisor already holds every
  read end and every PID. Putting the window there means one process owns both
  the display and the control. A separate GUI process would need yet another
  channel to receive state, and could show something different from what the
  supervisor believes.
* **Why can't the GUI just read the children's variables?** After `fork()`
  each child has its own copy of memory. A temperature changed in crushing
  only exists in crushing's address space; the only way the parent learns it
  is through the telemetry pipe.
* **How does telemetry change the colours?** Child sends a `MachineEvent` with
  its `state` -> `supervisor_poll(0)` reads it in the 33 ms timer ->
  `MachineView.state` is updated -> `display()` picks the colour from that.
* **How do the control signals work?** `kill(pid, SIGUSR1)` etc. The child's
  handler only sets a `volatile sig_atomic_t` flag; its main loop checks the
  flags each tick. Handlers have no `SA_RESTART`, so the signal also wakes the
  child from `poll()`/`nanosleep()` right away.
* **Why real-time signals for fault/clear?** Standard signals do not queue.
  Two quick deliveries of the same standard signal can merge into one;
  real-time signals are queued.
* **Why is the GUI never blocked?** `poll()` with timeout 0 checks the pipes,
  and only a pipe reported readable is read. Each message is smaller than
  `PIPE_BUF`, so a readable pipe always holds a whole message.
* **What does OpenMP actually benchmark?** The lab-side yield/quality
  integration per olive lot, not the process pipeline itself. See
  `docs/benchmark.md`.

### Design choices ("be ready to convince us")

* **Why pipes and not FIFOs?** Every process is a child of the supervisor, so
  anonymous pipes created before `fork()` reach everyone who needs them. A FIFO
  is a name in the filesystem: it is needed only for processes that are not
  related. It has to be created and removed, and it is left behind if the
  program crashes.
* **Why pipes and not shared memory?** The data is a stream of messages, not
  a table everyone edits. A pipe gives ordering, blocking and EOF for free.
  Shared memory would need semaphores and would not tell a stage that
  upstream has finished.
* **Why one telemetry pipe for all seven children instead of seven?** Each
  `MachineEvent` is smaller than `PIPE_BUF`, so every `write()` is atomic and
  messages never mix. One pipe means one descriptor to poll. The machine id
  inside the message says who sent it.
* **Why does QC get its data from the supervisor?** Otherwise every machine
  would need a second pipe to QC. The supervisor already receives everything,
  so it forwards a copy. The feed is non blocking, so a slow QC can never
  freeze the GUI; dropped samples are counted on screen.
* **Why `poll()` and not `select()`?** `poll()` takes an array of descriptors
  with no `FD_SETSIZE` limit, and it reports `POLLHUP` when all writers have
  closed, which is how the supervisor knows a channel is finished.
* **How does the line end by itself?** Reception closes its pipe after the
  last batch. The next stage's `read()` returns 0 (EOF), so it closes its own
  output and exits. The EOF travels down the line like a domino. This only
  works because every unused pipe end is closed after `fork()`: one forgotten
  write end and the reader would wait forever.
* **How are zombies avoided?** `waitpid(-1, &status, WNOHANG)` runs on every
  poll and collects any child that has exited. The status also shows *how* it
  ended (normal exit vs killed by a signal), which is how a crash is told
  apart from a normal finish.
* **What does Ctrl-C do?** The terminal sends `SIGINT` to the whole process
  group. The children ignore it; the supervisor's handler only sets a flag,
  then shuts the children down in order with `SIGTERM`, waits for them, and
  closes the window.
* **Why is so little done inside signal handlers?** Only async-signal-safe
  work is allowed there. Calling `printf` or `malloc` from a handler can
  deadlock if the signal interrupts the same function. So handlers only set a
  `volatile sig_atomic_t` flag, and the normal loop acts on it.
* **Why `_exit()` in the child and not `exit()`?** `exit()` runs `atexit`
  handlers and flushes stdio buffers copied from the parent, so the parent's
  buffered output would be printed twice and freeglut's cleanup would run in
  a process that never opened a window.
* **Why a config file?** The spec asks for no hard coded values. Every range,
  duration, temperature/pressure limit and the seed are read at start up, so
  a new scenario (like `hot_day.cfg`) needs no recompiling. Unknown keys are
  rejected so a typo is not silently ignored.
* **Same input, same output?** Batches are generated from the seed and the
  batch number, so two runs with the same config produce exactly the same
  production figures. `make test` checks this.
* **What if a pipe fills up?** Batches are about 60 bytes and a pipe holds
  64 KB, so a slow stage can have about a thousand batches waiting before the
  writer blocks. Blocking is the right behaviour then: it is backpressure, and
  the upstream machine waits like a real conveyor would.
