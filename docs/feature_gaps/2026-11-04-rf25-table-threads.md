# RF 2.5D: a Green's function's table made on several threads

The dense fill round's follow-up (`2026-11-04-rf25-dense-fill.md`, "Left"), the same day. Once ac3f5fc shared a dense
frequency's columns among its threads, a fresh frequency's fill was bounded by its slowest Green's function. The
frequency's Green's functions were made side by side, one to a thread, and each one's table of distances was made one
distance after another under its own lock in the kept store (`rf25_wave.cpp`'s `tableOf()`). On the reviewers' coplanar
line (`fgc_S62_L1000_r15p6_c10`) at 10 GHz, the walls' via kernel took 0.46 s, and the three pairs of heights took 0.14
to 0.21 s on threads of their own: 0.48 s of a 0.58 s fill at the dense LU round's mesh. A table's distances are
independent Sommerfeld integrals. They are now shared among the frequency's threads, each made by one of them alone,
and the frequency's Green's functions are made one after another, each table on all of its threads.

**In the app**, through `rf25_solve`: ac3f5fc's app and this change's, built alike in one worktree, on fgc_S62 at main's
graded mesh (4660 runs over 2336 cells), the dense limit set to 8000 in RF 2.5D > Settings… (`run_tables.sh` beside
this file, with the dense fill round's `bench.py`: the model and its layout copied for each solve, never its
`.solution`, the app's home, settings and caches in a scratch folder; the second pair of 1.5 GB sweeps by `bench.py`
alone, after first):

| fgc_S62, 4660 + 2336, 10 GHz | before (ac3f5fc) | now |
|---|---:|---:|
| a solve's first frequency, the tables made: its fill | 0.668, 0.649 s | **0.256, 0.256 s** (2.6×) |
| `rf25_solve`'s call | 3.70, 3.69 s | 3.28, 3.29 s |
| the same, the tables kept: its fill | 0.171–0.197 s | 0.172–0.175 s |
| its LU | 0.62–0.63 s | 0.59–0.62 s |
| a fresh 40-frequency sweep at the default memory (24 at a time, a thread each) | 24.96, 24.89 s | 25.13, 24.70 s |
| a fresh sweep in 1.5 GB (three at a time, eight threads each) | 27.3, 28.0, 29.7, 41.8 s | **25.0, 24.4, 24.5, 30.1 s** |
| its fill a frequency | 0.94, 0.91, 1.00, 1.28 s | 0.67, 0.64, 0.63, 0.87 s |

- **The answers are the same to the last bit.** The kept solutions' bodies are the same bytes before and after: one
  frequency's (253,058 bytes) in each of its three solves, the default sweep's (3,025,492) and the 1.5 GB sweep's
  (3,025,490). S and the sweep's dB in every answer are the same digits.
- **A first frequency's tables now take some 0.08 s** of its fill (0.256 against 0.172 kept), where they took 0.48.
- **At the default memory nothing changes**, as expected: 24 frequencies at a time leave each one thread, and its tables
  are made on it as before.
- **In 1.5 GB a fresh sweep is 8 to 18 % quicker** in the three pairs run at like loads, and 28 % in the fourth, each
  frequency's fill 0.27 to 0.41 s less. The LU, some 0.95 s a frequency three at a time, is now most of it.
- **The load**, from other sessions: 8 to 9 for the one-frequency runs, 20 to 35 for the default sweeps, and 13 to 34
  for the 1.5 GB ones. The 1.5 GB sweeps were run in pairs, before first twice and after first twice: the later pairs
  had the higher load (the last, 22 against 34, the widest apart).

## What it is

- **`tableOf(key, width, low, most, far, threads, node, ...)`** makes the distances it has to make through
  `nodesOf()`: an atomic counter shared by as many as `threads` threads (`onThreads()`, now included from
  `rf25_fast.h`), each distance made by one of them alone into its own place. Keeping nothing, they go into a list of
  its own; kept, the kept table's places are made first and its distances added once each has its value, the kernel's
  own lock held throughout as before: a second solve of a kernel still waits for the first and is handed its table. The
  store's byte count and its `built`/`reused` counts are as they were.
- **The furthest first.** A far distance's integral goes on along the vertical lines and takes some three times as long
  as a near one's, so the near ones are left to even out where the threads end. Twenty runs of each order, at a load of
  9: the four tables in 56.5 ms on 24 threads, 59.1 nearest first; 125.7 and 128.3 on 8 (medians).
- **`WaveGreen::prepare(threads)` and `ViaGreen::prepare(threads)`**, `threads` 1 unless given, return how many of the
  distances they made were made on threads other than the caller's. The first remainder asked for still prepares on
  its own thread.
- **`Kernels(c, f, threads)`** makes the frequency's Green's functions one after another again, as before ac3f5fc, each
  `prepare(threads)` on all of the frequency's threads, the first that cannot be made said. Side by side, a thread
  each, the four took as long as the slowest (the via's 0.46 s); one after another on all the threads they take their
  sum over the threads: 55.5 ms on 24 (below).
- **`fillShared().greens`** counts the tables' distances made on threads other than their frequency's own, where it
  counted the Green's functions.
- **The same to the last digit on any number of threads**: each distance is the same integral, made by the same
  expressions whichever thread makes it; nothing is summed across threads.
- **GCC**: the new lambdas name what they capture (none is long), and nothing returns a tuple. The four CI lints are
  green.

## Where the time was, and is

`probe.py` beside this file writes a test function into `test_rf25_wave.cpp` for the run, and takes it out again: the
model's stack (500 µm of silicon at 6.41 S/m, 20 µm of polyimide, the metal's 1.5 µm, no ground), its walls' via
through the metal and its three pairs of heights, at 10 GHz out to 1.1 mm. It reproduces the dense fill round's times
on one thread:

| fgc_S62's Green's functions at 10 GHz | distances | one thread | 4 | 8 | 16 | 24 threads |
|---|---:|---:|---:|---:|---:|---:|
| the walls' via | 197 | 451 ms | 114.5 | 57.1 | 29.2 | 25.1 |
| the three pairs of heights | 140, 178, 185 | 530 ms (136, 182, 212) | 135.2 | 67.4 | 34.9 | 30.4 |
| all four | | 980 ms | 250 | 124.5 | 64.1 | **55.5** |

- **Eight threads make them 7.9 times as fast, 16 some 15 times and 24 some 18.** The 24 are 16 performance cores
  and 8 efficiency cores; the load was 7 to 8 for this run, on the change as committed (furthest first).
- **A via's distance costs** some 1.0 ms in the nearest tenths of its table and 3.0 ms in the furthest; a pair's 0.35
  to 0.45 and 1.5 to 1.6.
- **What is left of a one-frequency call** of 3.3 s is its columns' fill (0.17 s), its LU (0.6 s) and some 2.4 s
  besides: the mesh, the static parts `fullWaveOf` makes once on one thread (the dense fill round found some 1.4 s of
  them at its coarser mesh), the answer.

## Tests

- **`test_rf25_wave::aTableIsTheSameOnAnyNumberOfThreads`**, new. The wave kernel on 254 µm of 9.8 at 1 GHz, and a via's
  of four components (two vias, two heights) through 100 µm under 50:
  - **Keeping nothing**, each made on one thread, then on four and seven: the same values (`==`), read at nought and at
    60 distances a decade from a nanometre to the table's end (the tables have 40 a decade, so every node is read
    beside its neighbours), and the same size. None made elsewhere on one thread, some on four and seven.
  - **Kept**: made on three threads (the store's `built` its size); handed over on five, nothing made and `reused` its
    size; grown on six from 2 to 10 mm, its further distances alone, the same as made on one thread to 10 mm.
  - **More threads than distances left**: the 10 mm table grown to 12.5 mm on 24 threads, a few distances, the same as
    one thread's.
  - **Two solves of one kernel at once**, each on three threads, from a common start: one makes the table and the other
    is handed it, nothing made twice (`built` and `reused` its size), both the one-thread values.
  - 0.82 s in Release, ten runs alike.
- **`test_rf25_fullwave::aFrequencysFillIsTheSameOnAnyNumberOfThreads`** holds as it was: the same z, S and currents on
  1, 4 and 7 threads, and now the tables' distances counted as made elsewhere on 4 and 7.
  **`aSolveReadsTheSameWhateverWasSolvedBefore`** passes, kept and not.
- **Release**: every rf25 suite and `test_macos_accelerate_weak`, 16 of 16, five at a time: 164 s on the final source,
  rebuilt after the breaks (178 s before the new test's two solves at once were added).
- **ASan and UBSan** (Debug, `QUCS_FEM_SOLVER=eigen`, as CI's Linux), on the final source: `test_rf25_wave` 152 s,
  `test_rf25_fullwave` 83 s and its second part 100 s, the three at once at a load of 7 to 12 (195, 115 and 137 s in
  a first run at 20 to 25); the new function 30.7 s alone. No sanitizer report. The dense fill round's run had the
  two parts at 273 and 319 s, two at a time. That is not measured here before and after, but their solves make
  their stacks' tables afresh, now on several threads.
- **The four CI lints**: `check-tool-arguments.py`, `check-json-nesting.py`, `check-third-party.py`, `check-shaders.py`.

## Breaks

`breaks.py` beside this file has 15 breaks. Each was built and its tests run, the sources put back whatever happened.
**All 15 were caught**, each by a test that holds the tables alike, the physics, or the counts, and each failing where
it was meant to, none by a crash. ("The new test" is `aTableIsTheSameOnAnyNumberOfThreads`; "the fill's test"
`test_rf25_fullwave::aFrequencysFillIsTheSameOnAnyNumberOfThreads`.)

| what was broken | caught by |
|---|---|
| A table's distances made on one thread | the new test (none made elsewhere on four); the fill's test (no distance made elsewhere) |
| Keeping nothing, a table's distances made on one thread | the new test |
| A kept table's distances made on one thread | the new test; the fill's test |
| Each distance made at the next one's | `theTableIsTheIntegral` (3.5e-5 at 0.73 µm); `aKeptTableIsTheOneItsKernelWouldMake`; `whatAViasClosedFormsLeaveIsSmallAndTabulated` (not the new test: one thread and several alike wrong) |
| The nearest distance to make left unmade | `aKeptTableIsTheOneItsKernelWouldMake` and the new test, the grown table not the one made whole (not `theTableIsTheIntegral`, which reads nothing at or below the first distance) |
| A kept table grown over its kept distances | `aKeptTableIsTheOneItsKernelWouldMake`; the new test (not `aSolveReadsTheSameWhateverWasSolvedBefore`, whose tables are kept to the further distance first and never grow) |
| The kernel's lock let go while its distances are made | the new test: of two solves at once 212 distances built, 0 handed over |
| The store's bytes of the table before it grew | `aKeptTableIsTheOneItsKernelWouldMake` (nothing kept within its budget) |
| What other threads made not returned, the wave kernel's | the new test |
| The same, a via's | the new test |
| The caller's own distances counted as made elsewhere | the new test; the fill's test (2003 counted on one thread) |
| A frequency's tables made on one thread (`Kernels`) | the fill's test's counts |
| Every via's kernels made for the stack's first via | `aViaBetweenTwoMetalsJoinsThem` (not the faces' test or the via to the ground: one via of the stack each) |
| The pairs of heights' Green's functions read the other way round | `linesOnTwoMetalsAreSolvedTogether` (not the faces' test: its two heights 3 µm apart, their Green's functions near alike; nor the fill's test, its two solves alike) |
| Every Green's function that cannot be made said, not the first | the fill's test (the refusal said once) |

- **Not a break**: the distances handed out nearest first. No test can see it, since the tables are the same and only
  the time differs. It was measured instead (above).
- **Rebuilt after**: both builds were made again from the restored sources, and the tests below were run on them.

## Left

- **The static parts `fullWaveOf` makes once**, on one thread, now the most of a one-frequency call: being shared
  among threads in a change of its own.
- **The LU** is now most of a dense frequency in a sweep: some 0.95 s a frequency three at a time in 1.5 GB.
- **Not measured**: GCC's build (CI's); Intel Macs; machines of fewer cores.
