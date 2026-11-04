# RF 2.5D: a dense frequency's fill shared among its threads

The follow-up the dense LU's round left (`2026-11-04-rf25-dense-lu.md`, "Left"), taken up the same day. Once Accelerate
factorized the full wave's runs × runs system (9edbb9d), a frequency of the dense solve was mostly its fill, on one
thread: on the reviewers' largest model (`fgc_S62_L1000_r15p6_c10`, 3588 runs over 1800 cells) its fill took 1.6 s and
its LU 0.32 s. A sweep at the dense limit ran two or three frequencies at a time (`kSweepMemory`, 1.5 GB, over a
frequency's `16 (2 nr² + nc²)` bytes), so 21 or 22 of the machine's 24 threads waited. The compressed solve already
gave each frequency `threads / side` of them; the dense one now does too.

Main moved on twice while this was done. 47798db meshes this model more finely (past the dense limit), and ac435e2
(RF 2.5D > Settings…) lets a sweep hold a quarter of the machine's memory, 16 GB here. With that default, a dense
sweep on this machine runs 24 frequencies at a time, one thread each, and nothing is left to share. The fill is shared
where fewer frequencies run at once than there are threads: one frequency, a short sweep, a machine with less memory,
a budget set lower, a larger system. Both bases were measured (below); the change is committed on ac435e2.

**In the app**, through `rf25_solve` (9edbb9d, with and without the change, at the dense LU round's mesh; below):

| fgc_S62, 3588 + 1800 | before | now |
|---|---:|---:|
| one frequency, 10 GHz: its fill, the stack's tables kept | 1.60 s | **0.113 s** (14×, 24 threads) |
| its fill, the tables made (a solve's first) | 2.63 s | 0.58 s (4.6×) |
| its LU | 0.33 s | 0.34 s |
| `rf25_solve`'s whole call, tables kept | 3.32 s | 1.83 s |
| its 40-frequency sweep, the tables made (a fresh app) | 44.3 s | **18.5 s** (2.4×) |
| the same sweep again, the tables kept | 30.4 s | 11.8 s (2.6×) |

- **The answers are the same to the last bit**: S, Z, the currents and everything else kept, at one frequency and over
  the whole sweep (below).
- **Memory is as it was**: the same matrices, made where they were. The app's peak footprint (`/usr/bin/time -l`) was
  613 and 610 MB for one frequency, 1196 and 1186 MB for the sweep.
- **Left as they were**, as asked: `kMostUnknowns` (6000), the sweep's memory and a frequency's term (ac435e2's now).
- **On main's head** (fgc_S62 at its new mesh, 6996 runs and cells, the dense limit set to 8000): one frequency's fill
  2.70 → 0.17 s; the sweep at the default memory, 24 at a time, 25.2 → 23.9 s; in 1.5 GB, three at a time, 69.4 →
  28.0 s.

## What it is

- **`solveAt(c, f, threads)`** fills the system M and the cells' potentials P a column at a time, each column by one of
  the frequency's threads and by no other, as they come free (`sharedOut()`, an atomic counter as the frequencies and
  the compressed blocks are shared):
  1. **Each column's part above the diagonal and on it**, the longest first so that the threads end together.
     M's columns: a run in the plane's jw L with the runs before it (`systemColumn()`); a via's coupling to every run in
     the plane, and to the vias up to itself, the two ways round averaged as before. P's: each cell's potential of a
     charge spread over another (`potentialColumn()`).
  2. **The part below**, made from the others' above (`mirrorColumns()`), a block of 16 columns to a thread so that each
     of a column's rows above is read in one piece.
  3. **The charges**, A^T P A / jw, added a column at a time (`chargeColumn()`), which read P whole.
  4. Then, on the frequency's own thread as before: each run's own impedance, the vias' potential at the ports, P J,
     the right-hand sides and the ports' paths (some 10 ms of it).
- **Columns, where rows were asked for.** Both matrices are symmetric, so a column is its row; Eigen holds a
  matrix by columns, so a column is in one piece. A thread writing rows would have written into every cache line of
  the matrix (eight rows to a 128-byte line), each line shared with the threads writing the neighbouring rows. It would
  also have read P across its rows. The charges were added a row at a time, reading P across its rows. Added a column
  at a time, they read two of P's columns, tail(b)'s and head(b)'s, in one piece.
- **The same to the last digit on any number of threads.** Each term is made by the expression that made it, with its
  arguments the same way round, in the same order: the inductance before the charges, the charges before the run's own
  impedance. The part below the diagonal is the part above it copied, as it was before (`m(a, b) = m(b, a) = ...`). No
  sum is split among threads.
- **`Kernels(c, f, threads)`**: a frequency's Green's functions — one for each pair of metal heights, one for each of
  the stack's vias — are made side by side, one to a thread, where their tables are not kept from an earlier solve.
  They are taken in the order one thread made them in, and the first that cannot be made is said, alone, as before.
  The compressed solve's kernels are made this way too: the same constructor.
- **`fullWaveOf`** gives each frequency `inner = threads / side` threads, as the compressed path did, and says `threads`
  as `side × inner` for both. (`side` is ac435e2's: the sweep's memory over a frequency's `16 (nr² + nc²)`.) A
  one-frequency dense solve now says 24, where it said 1; `rf25_solve` returns it.
- **`onThreads()`** moved from `rf25_fast.cpp`'s anonymous namespace to `rf25_fast.h`, one helper for both solves.
- **`fillShared()`**, for tests, counts the Green's functions and the columns (or blocks of them) made on threads
  other than their frequency's own: a fill quietly on one thread gives the same answer, so only a count can tell.
- **GCC** (CI's Linux and Windows; there is none here): every new lambda names what it captures, none is long, and
  nothing returns a tuple. `rf25_fullwave.cpp` and `rf25_fast.cpp` compile without a new warning under clang's `-Wall
  -Wextra -Wpedantic -Wshadow` (its one `-Wshadow` warning, `fullWaveOf`'s `z`, is older).

## Where the time was, and is

Phases timed in a build of each with `fprintf` at their ends (`phases_before.py` and `phases_after.py` beside this
file, applied to a copy of the source and never committed), `rf25_solve` at 10 GHz:

| fgc_S62 at 10 GHz | before, one thread (load 12.6) | now, 24 threads (load 21) |
|---|---:|---:|
| the Green's functions, made | 1.00 s: three pairs of heights 0.14, 0.19 and 0.21; the walls' via 0.46 | 0.48 s |
| the same, kept | 0 | 0 |
| jw L between the runs in the plane | 0.33 s | } 0.113–0.122 s |
| the vias' couplings | 0.12 s | } |
| P | 0.96 s | } |
| the part below the diagonal | (made with the part above) | 0.004–0.011 s |
| A^T P A / jw | 0.25 s | 0.016–0.019 s |
| the rest (own impedances, P J, right-hand sides, paths) | 0.006 s | 0.009–0.010 s |
| the LU (the factors, the solve, rcond) | 0.33 s | 0.41–0.46 s |

- **The columns, the mirror and the charges together**: 1.66 s on one thread → 0.13–0.15 s on 24, some 12 times.
  The 24 threads are 16 performance cores and 8 efficiency cores, and other sessions kept the load at 9 to 21.
- **A fresh frequency's fill is now its slowest Green's function**: the walls' via kernel, 0.46 s, whose table of
  distances is made one node after another under its own lock (`rf25_wave.cpp`'s `tableOf()`).
- **The LU is now most of a dense frequency** in a sweep: 0.49 s of a kept frequency's 0.75 s.

## In the app

**How it was run.**
- **The base.** Main had moved on as this began: 47798db grades a full-wave mesh at narrow gaps, which takes this model
  to 4660 runs over 2336 cells, past the dense limit, so `rf25_solve` refuses it dense there. `solveAt` and `Kernels`
  are the same in both commits (47798db adds a comment above `crowding()`). So the timings are of 9edbb9d's app with
  and without this change, built alike in a detached worktree, at the dense LU round's 3588 runs over 1800 cells. The
  change is committed on ac435e2, main's head by then, and built, tested and timed there too (below).
- **The driver** (`bench.py`, `run_timings.sh` beside this file). The app's MCP server was driven headless:
  `QT_QPA_PLATFORM=offscreen`, with home, settings, cache and trash in a scratch folder. The model and its layout were
  copied from the benchmark's folder for each solve, never its `.solution`. Nothing was written to
  `~/QucsWorkspace` or the user's caches.
- **The calls.** `rf25_solve` with `solver: full_wave`, `matrix: dense`, and `frequency: 10[GHz]` or the model's own 40
  frequencies (1 to 40 GHz). The figures are its `matrix` answer's `fill s` and `LU s` (a frequency's, on its thread),
  and the call's wall time.
- **The runs.** Four one-frequency solves in each app process (the first makes the stack's tables, the rest keep them)
  and two sweeps in each, the two apps in turn, two processes of each. Other sessions kept the load at 8.6 to 10.6
  through the one-frequency runs and the first round of sweeps, and up to 24 in the second round.

**One frequency at 10 GHz** (2 first solves and 6 kept for each app; the load 8.6 to 9.2):

| | before | now |
|---|---:|---:|
| fill, the tables made | 2.626, 2.630 s | 0.576, 0.577 s |
| fill, kept | 1.592–1.616 s (median 1.601) | 0.111–0.113 s (median 0.113) |
| LU | 0.322–0.337 s | 0.322–0.351 s |
| `rf25_solve`'s call, made | 4.42, 4.43 s | 2.37, 2.38 s |
| `rf25_solve`'s call, kept | 3.31–3.33 s | 1.83–1.85 s |

- **What the call is besides**: about 1.4 s of the 1.83, in either app. That is the solve's mesh, read once, and the
  static inductances and potentials `fullWaveOf` makes once a solve, on one thread, before its frequencies (the dense
  LU's write-up found 1.4 to 1.5 s). It is now the most of a one-frequency solve.

**The sweep**, 40 frequencies three at a time (`side`), each frequency now on eight threads:

| | before | now |
|---|---:|---:|
| round 1 (load 8.6–10.6), the tables made: the sweep | 44.3 s | 18.5 s |
| its fill and LU a frequency | 2.65 and 0.44 s | 0.73 and 0.49 s |
| round 1 again, the tables kept: the sweep | 30.4 s | 11.8 s |
| its fill and LU a frequency | 1.63 and 0.47 s | 0.26 and 0.49 s |
| round 2 (load 11–24), made / kept | 47.8 / 35.5 s | 22.0 / 15.4 s |

- **The before matches the dense LU round's 44.2 s** for the sweep, and its 2.65 s fill a frequency on its thread.
  That fill was the stack's tables, made at each frequency: kept, a frequency's fill in the sweep is 1.63 s, as one
  frequency's alone is (1.60 s). Three fills at once are not 1.6 times one alone, as that round read it.
- **The LU a frequency rose** from 0.44 to 0.49 s: three fills at once now take every thread while another frequency
  factorizes.

**fgc_S51_L2000_r1p8, 1468 runs over 740 cells**: its 40 frequencies 19 at a time, which leaves each frequency one
thread, so its fill is on one thread, as before (the load 28 throughout, from other sessions):

| | before | now |
|---|---:|---:|
| the sweep, the tables made | 8.40 s (fill 2.98 s a frequency) | 7.86 s (2.71 s) |
| the sweep, kept | 2.34 s (fill 0.61 s) | 2.11 s (0.53 s) |

- **Not slower on one thread.** The charges added a column at a time, reading P's columns in one piece, are the likely
  difference, though one run of each at that load is no measurement of it.

## On main's head

ac435e2, built with and without this change in the same detached worktree (`run_main.sh` beside this file). There,
fgc_S62 meshes to 4660 runs over 2336 cells, past the automatic dense limit, so RF 2.5D > Settings…'s limit was set to
8000 for these runs. The load was 10 to 13 for the one-frequency runs and 15 to 29 for the sweeps.

| fgc_S62, 4660 + 2336 | before | now |
|---|---:|---:|
| one frequency at 10 GHz: fill, kept | 2.68, 2.71 s | 0.170, 0.174 s |
| fill, the tables made | 3.69 s | 0.63 s |
| LU | 0.60–0.62 s | 0.61–0.62 s |
| `rf25_solve`'s call, kept | 5.61 s | 3.10 s |
| the sweep at the default memory (16 GB here: 24 at a time, one thread each) | 25.2 s (fill 4.98 s, LU 6.76 s a frequency) | 23.9 s (4.48 s, 6.48 s) |
| the sweep in 1.5 GB (three at a time, eight threads each) | 69.4 s (fill 3.87 s, LU 0.93 s) | 28.0 s (0.90 s, 0.95 s) |

- **At the default memory nothing changes**: each frequency has one thread, its fill as before. Its 24 LUs at once
  share the machine's matrix units, 6.5 to 6.8 s each where one alone is 0.6.
- **With the fill shared, three at a time in 1.5 GB comes within 17 % of 24 at a time in 16 GB** (28.0 against 23.9 s).
  The memory a sweep may hold is ac435e2's setting, and was left as it is.
- **The answers are the same to the last bit** here too: the one frequency's kept bodies (253,058 bytes) one byte apart
  (threads), the default sweep's (3,025,492 bytes) none, the 1.5 GB sweep's one (threads 3 against 24).

## The answers

- **One frequency.** S, Z and the checks in `rf25_solve`'s answer are the same in every pair of the four processes
  (the JSON's doubles are shortest round-trip, so the same digits are the same bits). The kept solutions'
  decompressed bodies are 194,146 bytes each, and one byte apart: `FullWave::threads`, 1 against 24. z, s, every
  current and every other member are the same bytes. (`compare.py` beside this file reads the files' heads and
  bodies.)
- **The sweep.** The bodies are 2,310,130 bytes and one byte apart (threads 3 against 24): the 40 frequencies' z, s and
  currents to the last bit. Every S term in dB over the sweep, in the JSON, is the same too.
- **fgc_S51's sweep**: 898,426 bytes, no byte apart (19 threads either way).

## Tests

- **`test_rf25_fullwave::aFrequencysFillIsTheSameOnAnyNumberOfThreads`**, new, in the suite's first part. A copper
  line, solved as its two faces (each face's twin, and the walls' vias between them), is shorted to the ground by a via
  at its far end and driven at its edge and its middle (the ports' paths up from the ground). Its runs go along it and
  across it.
  - **Two frequencies**, solved on 1, 4 and 7 threads: the same z, S and currents exactly (`==`), and `threads` said as
    `side × (threads / side)`, 4 and 6.
  - **The counts**: on one thread no Green's function and no column is made elsewhere; on four and seven, both are.
  - **What cannot be made is said as on one thread.** A metal right on the ground plane: one problem, the first pair of
    heights' ("shorted to it"), the same on 1 and 4 threads.
  - **Held compressed** on four threads: its Green's functions shared out alike, no column counted, and S within 1e-5
    of the dense solve's. (Its products are summed a thread at a time, so it is not the same to the last digit.)
  - **A sanitized build** solves the line as a sheet, at one frequency, on one thread and four, and leaves out the
    compressed solve. With the faces' three pairs of heights and two vias' kernels made afresh for each solve, it was
    101 s under ASan with Eigen's LU; it is 10 s.
- **Release.** Every rf25 suite and `test_macos_accelerate_weak`, five at a time: 16 of 16 (221 s). On the final
  source, `test_rf25_fullwave` whole again: 26.0 s and its second part 25.4 s. `test_qucs_control`'s four rf25
  functions, and the four lints.
- **ASan and UBSan** (Debug, with `QUCS_FEM_SOLVER=eigen`, as CI's Linux): `test_rf25_fullwave` 273 s and its second
  part 319 s on the final source, two at a time; earlier, with `test_rf25_fast` 48 s and `test_rf25_deembed` 332 s,
  four at a time. No sanitizer report.
- **On ac435e2 with the change**, as committed: every rf25 suite and `test_macos_accelerate_weak` 16 of 16 in Release
  (239 s, `test_rf25_doc`'s new settings functions among them), `test_qucs_control`'s four rf25 functions and the four
  lints; `test_rf25_fullwave` 212 s and its second part 281 s under ASan and UBSan. No sanitizer report.

## Breaks

`breaks.py` beside this file has 21 breaks. Each was built and its tests run, the sources put back whatever happened.
They ran on a copy of the source: a detached worktree of 47798db with this change, built apart (on ac435e2 the change
is the same, the sweep's comment apart). **All 21 were caught**,
each by a test that holds the physics, the solve made apart, or the counts. ("The new test" is
`aFrequencysFillIsTheSameOnAnyNumberOfThreads`.)

| what was broken | caught by |
|---|---|
| The Green's functions made on one thread | the new test's counts |
| Every via's kernels made for the stack's first via | `aViaBetweenTwoMetalsJoinsThem` (not the new test, whose two solves are alike; not the faces' or the via to the ground, one via of the stack each) |
| The pairs of heights' Green's functions taken the other way round | `linesOnTwoMetalsAreSolvedTogether` (not the faces': their two heights 3 µm apart, their Green's functions near alike) |
| Every Green's function that cannot be made said, not the first | the new test (the refusal said once) |
| The compressed solve's Green's functions made on one thread | the new test's counts |
| The system's and P's columns made on one thread | the new test's counts |
| A frequency's fill given one thread | the new test's counts and threads |
| The threads said as the frequencies at a time | the new test |
| `onThreads` running the caller's share alone | the new test; `anHMatrixMultipliesAsItsDenseMatrixDoes` (not `aCompressedSolveIsTheDenseOne`, whose products are too small to be shared) |
| A run's own term with itself left unmade | `atLowFrequencyItIsTheQuasiStaticExtraction`; `aCompressedSolveIsTheDenseOne`; the new test |
| A via's coupling to a run, the run's head and tail the other way round | `aViaToTheGroundIsItsOwnInductance`; `aViaBetweenTwoMetalsJoinsThem` |
| A via's own term with itself left unmade | `aViaToTheGroundIsItsOwnInductance`; `aViaBetweenTwoMetalsJoinsThem` |
| Two vias' coupling taken one way round, not the two averaged | the new test: its compressed solve, made apart, 4.3e-4 from the dense one (not the via tests, nor `aCompressedSolveIsTheDenseOne`: their vias' two ways round differ by less than they hold to) |
| The charges' column, its own tail and head the other way round | `atLowFrequencyItIsTheQuasiStaticExtraction`; `aJunctionIsReciprocalAndPassive` |
| The mirror's first row of each block left out | `aJunctionIsReciprocalAndPassive`; `atLowFrequencyItIsTheQuasiStaticExtraction` |
| Each block's columns past its first left unmirrored | the same two |
| P left unmirrored | the same two |
| The charges added before the matrices are mirrored | the same two |
| Each column mirrored as it is made, the others' it reads perhaps still being made | the new test (four threads not as one) |
| A Green's function made on another thread not counted | the new test |
| A column made on another thread not counted | the new test |

- **A first run caught all 21 too, one of them by chance.** It named `theSheetAndTheScatteringMatrixAreTheirClosedForms`
  for the columns' and the mirror's breaks, and that test solves no system. "A run's own term with itself left unmade"
  then fell to the new test alone: its unmade diagonal held other memory on four threads than on one. The seven
  breaks' tests were named again, and those seven run again (`BRK_ONLY=10,13,14,15,16,17,18`).
- **Rebuilt after**: the copy's build was made again from its sources, which are this change's, and the breaks' tests
  passed on it.

## Left

- **The static parts a solve makes once**, on one thread, in `fullWaveOf`: each pair of runs' aligned partial
  inductance (direct, and of the image) and each pair of cells' static potential. About 1.4 s of a one-frequency
  `rf25_solve` of 1.83 s on fgc_S62. The same column sharing would do for them.
- **A Green's function's table**, made one distance after another (`tableOf()`): the walls' via kernel's 0.46 s is now a
  fresh frequency's fill. Its nodes are independent of one another.
- **`kMostUnknowns` and the sweep's memory term**, the user's call. On the numbers here, the limit's case (6000 runs and
  cells, some 4000 runs) would take 0.14 s of fill and 0.46 s of LU a frequency alone; 8500 some 0.28 and 1.3 s. That
  is the fill as N² and the LU as nr³ from fgc_S62's, both extrapolated.
- **The sweep's memory**, ac435e2's: with the fill shared, a sweep holding an eighth of the default memory took 17 %
  longer on this machine.
- **Not measured**: GCC's build (CI's); Intel Macs; machines of fewer cores, where `side` takes the threads before
  `inner` does.
