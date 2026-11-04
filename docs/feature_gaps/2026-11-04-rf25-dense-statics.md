# RF 2.5D: a dense solve's static parts shared among its threads

The follow-up the dense fill's round left (`2026-11-04-rf25-dense-fill.md`, "Left"), taken up the same day. Once
ac3f5fc shared a frequency's fill among its threads, most of a one-frequency dense solve was what `fullWaveOf` makes
once before its frequencies, on one thread. That is the runs' aligned static partial inductances, direct and of their
images (`alignedInductance()` for each two runs not at right angles), and each two cells' static potential
(`rectanglePotential()`). On the reviewers' largest model (`fgc_S62_L1000_r15p6_c10`, at main's mesh 4660 runs over
2336 cells) they took 2.2 s of a 3.1 s `rf25_solve`. They are now shared out among all of the solve's threads the way
ac3f5fc shares a frequency's fill: `sharedOut()` over `onThreads()`, each column by one thread alone.

**In the app**, through `rf25_solve` at 10 GHz (ac3f5fc's app with and without this change, below):

| fgc_S62, 4660 + 2336 | before | now |
|---|---:|---:|
| the static parts (one thread → 24) | 2.21–2.23 s | **0.135–0.141 s** (16×) |
| `rf25_solve`'s whole call, the stack's tables kept | 3.00–3.10 s (median 3.06) | **0.94–0.98 s** (median 0.97) |
| the call, the tables made (a solve's first) | 3.60, 3.63 s | 1.50, 1.55 s |
| its frequency's fill / LU | 0.16 / 0.58–0.65 s | 0.16 / 0.58–0.63 s |

- **The answers are the same to the last bit**: the kept solutions' bodies byte for byte, at one frequency and over the
  40-frequency sweep, and each static matrix whole (below).
- **Memory as it was**: the same three matrices. Each term is now written once; the inductances used to be zeroed
  first.
- **Left as they were**, as asked: `kMostUnknowns` and RF 2.5D > Settings…'s defaults.

## What it is

- **`makeStatics(c, threads)`**, called by `fullWaveOf` where the statics were made, before the frequencies are
  shared out, with all of the solve's threads (`FullWaveControl::threads`, or the machine's):
  1. **The columns**, one to a thread as they come free (`sharedOut()`): first each column of the potentials whole
     (`staticPotentialColumn()`), which are not symmetric (cell i's potential of a charge spread over cell j is not
     cell j's of one over cell i where the two differ in size); then each column of the inductances above the diagonal
     and on it (`staticInductanceColumn()`), the longest first. So the last columns handed out are the shortest, and the
     threads end together.
  2. **The inductances' part below the diagonal**, copied from the others' above (`mirrorColumns()`, now a template
     on the matrix type), sixteen columns to a thread, as ac3f5fc mirrors a frequency's M and P.
- **Columns**, as ac3f5fc's: Eigen holds a matrix by columns, so each thread writes its own memory in one piece. The
  old loops went across the matrices' rows: the inductances wrote both halves of each pair at once, and the potentials
  went a row at a time.
- **The same to the last digit on any number of threads.** Each term is made by the expression that made it, with its
  arguments the same way round: `alignedInductance(c.runs.at(a), c.runs.at(b))` and `(c.runs.at(a), c.mirrored.at(b))`
  for a ≤ b, the cosine test as it was, and `rectanglePotential(c.rect.at(j), c.centre.at(i), |z_i - z_j|)` (the cells'
  heights read from `c.cellZ`, the copy of the list the old loop read). The part below is the part above copied, as
  before. No sum is split among threads.
- **No zeros first.** The inductances were `Zero()`ed and the pairs at right angles skipped; now each term is written
  once, nought where the runs are at right angles. So the 347 MB of the two inductance matrices are first touched
  on all the threads, not zeroed on one.
- **`fillShared().statics`** counts, for tests, the static parts' columns and blocks made on threads other than the
  caller's (`sharedOut()` takes the counter, `sharedColumns` by default).
- **GCC** (CI's Linux and Windows; there is none here): every new lambda names what it captures, none is over six
  lines, and nothing returns a tuple. `rf25_fullwave.cpp` compiles with no new warning under clang's `-Wall -Wextra
  -Wpedantic -Wshadow` (its one `-Wshadow` warning, `fullWaveOf`'s `z`, is older).

## Where the time was, and is

Timed in a build of each app with `fprintf` at the ends of the phases (`phases_before.py` and `phases_after.py` beside
this file, applied to a copy of the source and never committed), three solves each, the load 8.5 to 9.2:

| fgc_S62 at 10 GHz | before, one thread | now, 24 threads |
|---|---:|---:|
| `fullWaveOf` up to the static parts (the runs, the cells, the ports' paths) | 0.015 s | 0.015 s |
| the inductances' zeros | 0.021 s (the first solve), 0.003 s | — |
| the inductances, direct and of the images | 1.66–1.67 s | } 0.131–0.135 s |
| the potentials | 0.54–0.55 s | } |
| the inductances' part below the diagonal | (with the part above) | 0.004–0.008 s |
| **the static parts** | **2.21–2.23 s** | **0.135–0.141 s** |

- **What the call is besides `fullWaveOf`**: some 0.05 s in either app, 0.12 s for a solve's first (the call's wall
  time less `fullWaveOf`'s end). So the "about 1.4 s of mesh and static parts" the dense fill's round saw at 3588 runs
  was nearly all static parts. At this mesh they were 2.2 s.
- **A one-frequency solve is now its frequency**: of the 0.97 s, the static parts 0.14 s, the frequency's fill 0.16 s
  and its LU 0.6 s. A fresh solve's Green's tables are 0.5 s more (`tableOf()`, made one node at a time: the dense
  fill's other follow-up). (The phase builds' own `fullWaveOf` is some 0.05 s longer than the apps': it hashes the
  static matrices, 391 MB, on one thread.)

## In the app

**How it was run.**
- **The base.** ac3f5fc (`claude/wonderful-jepsen-b8dac4`, the dense fill, on ac435e2), with and without this change,
  built alike in this worktree. Main has since moved on with Multiphysics 3D's tools (501af64), which change none of
  these files.
- **The driver**: the dense fill's `bench.py` (`../2026-11-04-rf25-dense-fill/`), through `run_timings.sh` beside this
  file. The app's MCP server was driven headless (`QT_QPA_PLATFORM=offscreen`, home, settings, cache and trash in a
  scratch folder). The model and its layout were copied from the benchmark's folder for each solve,
  never its `.solution`. Nothing was written to `~/QucsWorkspace` or the user's caches (`find -newer` after).
- **The calls.** `rf25_solve` with `solver: full_wave`, `matrix: dense`, `frequency: 10[GHz]`, or the model's 40
  frequencies (1 to 40 GHz). The model meshes to 4660 runs over 2336 cells since 47798db, past the automatic dense limit
  (6000 runs and cells), so RF 2.5D > Settings…'s limit was set to 8000 for every run (`BENCH_INI`).
- **The runs.** Four solves in each app process (the first makes the stack's tables, the rest keep them), the two
  apps in turn, two processes of each, the load 9.2 to 11 from other sessions.

**One frequency at 10 GHz** (2 first solves and 6 kept for each app):

| | before | now |
|---|---:|---:|
| `rf25_solve`'s call, the tables made | 3.63, 3.60 s | 1.50, 1.55 s |
| the call, kept | 3.00, 3.05, 3.06, 3.06, 3.09, 3.10 s | 0.94, 0.97, 0.97, 0.97, 0.98, 0.98 s |
| fill, made / kept | 0.63–0.64 / 0.157–0.170 s | 0.63–0.64 / 0.157–0.162 s |
| LU | 0.575–0.645 s | 0.582–0.626 s |

**The sweep**, the model's 40 frequencies at the default memory (16 GB here: 24 frequencies at a time, one thread
each), one sweep of each app in turn, three rounds:

| round (the load after each sweep: other sessions' and its own 24 threads) | before | now |
|---|---:|---:|
| 1 (22, 37) | 24.5 s | 28.1 s |
| 2 (32, 41) | 29.2 s | 25.6 s |
| 3 (45, 50) | 27.3 s | 24.4 s |

- **The static parts are made once a sweep**: 2.1 s of its 24 to 29. Other sessions' load moved a sweep by more than
  that. In the first round the later sweep, at the higher load, went the other way.

## The answers

- **One frequency.** The kept solutions' decompressed bodies (`compare.py` beside the dense fill's write-up) are
  253,058 bytes each and **no byte apart**: the first solve and two kept ones of one pair of processes, and a kept one
  of the other pair. Both apps say 24 threads, so not even that byte differs. Only the stamp in the head differs.
- **The sweep.** 3,025,492 bytes each, no byte apart.
- **The static matrices whole.** The phase builds hash each one (FNV-1a over its words) once it is made. All six
  solves, three of each app, give the same three hashes: `lDirect` 74134a13a226ab23, `lImage` 499e46927a44fe0a,
  `pStatic` 58b74de04525acdb. That includes the inductances' part below the diagonal, which nothing reads (below).

## Tests

- **`test_rf25_fullwave::theStaticPartsAreTheSameOnAnyNumberOfThreads`**, new, in the suite's first part. It uses
  `aCompressedSolveIsTheDenseOne`'s strip at a slant: 517 runs, 178 of them oblique, over 306 cells of unequal size, a
  lossy copper sheet on 100 µm of 9.8, at 5 GHz.
  - **Dense on one thread and on four**: the same z, S and currents exactly (`==`), and `threads` said as 4.
  - **The counts**: on one thread none of the static parts is made elsewhere. On four, other threads made 333 to 707
    of their 889 columns and blocks, in ten runs here in Release and under ASan. A compressed solve makes none.
- **Why a new test, and not three lines in `aFrequencysFillIsTheSameOnAnyNumberOfThreads`.** That test's line has
  182 static columns and blocks, 1.6 ms of work on one thread (measured with `probe.py` beside this file). Other
  threads took 97 to 165 of them on 4 and 7 threads here. But CI's macOS runner has three cores and runs three suites
  at a time: a thread started more than 1.6 ms late would find nothing left, and the count would fail. The slant's
  parts are 45 to 60 ms, some 30 times the margin, and the test takes 0.4 s in Release. (A first version put the counts
  in the line's test. It passed here, and was not kept.)
- **Release**: `test_rf25_fullwave` whole through ctest, its two parts at once. 19.7 and 19.8 s on the first version;
  24.0 and 24.1 s on the final source, rebuilt after the breaks, with the ASan run beside it.
- **ASan and UBSan** (Debug, `QUCS_FEM_SOLVER=eigen`, as CI's Linux): the two parts at once, 298.6 and 345.3 s with
  the breaks running beside them (189.7 and 247.9 s on the first version, alone). No sanitizer report. The new
  function alone takes 18.9 s; CI's Linux, about twice this machine's time, gives a function 900 s.
- **The four lints** (`check-tool-arguments`, `check-json-nesting`, `check-third-party`, `check-shaders`) pass.

## Breaks

`breaks.py` beside this file has 13 breaks. Each was built in this worktree's Release build and its tests run, the
sources put back whatever happened. The build was made again from the final sources afterwards, and both parts passed
on it. **All 13 were caught.** ("The new test" is `theStaticPartsAreTheSameOnAnyNumberOfThreads`.)

| what was broken | caught by |
|---|---|
| The static parts made on one thread | the new test's counts |
| The static parts made on two threads where one is asked | the new test (the one thread's count) |
| The static parts made for a compressed solve too | the new test (the compressed solve's count) |
| The static parts counted as a frequency's columns | the new test's counts |
| `sharedOut` counting every job as a frequency's column, whatever it is given | the new test's counts |
| `sharedOut` counting no job | the new test; `aFrequencysFillIsTheSameOnAnyNumberOfThreads` |
| A run's own static inductance with itself left unmade | `atLowFrequencyItIsTheQuasiStaticExtraction` (C 0.203 pF against 0.348); `aCompressedSolveIsTheDenseOne` (nothing finite at 10 MHz). Not the new test: both of its solves are broken alike |
| The inductances' columns made below the diagonal, where the mirror writes | the same two |
| Parallel runs' static inductance left nought, and those at right angles made | the same two (L' 0.69 nH/m against Hammerstad and Jensen's 422) |
| The direct static inductance the image's | the same two (the same 0.69: either way a run's static term cancels its image's) |
| The image's static inductance the run's own | the same two (not `aViaToTheGroundIsItsOwnInductance`) |
| The static potentials of two cells taken at one height | `linesOnTwoMetalsAreSolvedTogether`; `aMetalSolvedAsItsTwoFacesHoldsItsWallsFieldAndSharesItsCurrent`; `aViaBetweenTwoMetalsJoinsThem` |
| The static potentials' columns cut at their diagonal | `atLowFrequencyItIsTheQuasiStaticExtraction`; `aJunctionIsReciprocalAndPassive`; the new test (the part below left as memory held it, which differed between the solve on four threads and the one on one) |

- **Not listed: breaks of what no solve reads.** These were reasoned through, not run:
  - the mirror left out, or the image's part below copied from the direct one: nothing reads the part below (Left);
  - the potentials' columns written transposed: `Kernels::potential()` averages the two ways round, so only the last
    rounding moves;
  - the right-angle test dropped: `systemColumn()` skips runs at right angles itself;
  - the columns handed out in another order.

  Each leaves every answer as it is (or moves a last rounding), so no test could catch it.

## Left

- **The inductances' part below the diagonal is read by nothing.** `systemColumn()` reads `lDirect(a, b)` and
  `lImage(a, b)` for a ≤ b alone. It is made as it was (the mirror, 4 to 8 ms of the 0.14 s), so the matrices stay
  whole and symmetric. But no test can tell it is there, so no break of it is listed: the hashes above checked it.
  Held as their upper triangles alone, packed, the two matrices would take half the memory they do: 174 MB less at
  fgc_S62, of the solve's 391 MB of static parts. That changes how they are held, and is not done here.
- **A one-frequency solve is now mostly its LU** (0.6 of 0.97 s), and a fresh one's Green's tables (0.5 s, `tableOf()`).
- **`aFrequencysFillIsTheSameOnAnyNumberOfThreads`'s own counts** (Green's functions and columns made elsewhere) are
  of the same small line as its statics' were. Their margin on a three-core runner was not measured here.
- **`kMostUnknowns` and the sweep's memory**, the user's call, as they were.
- **Not measured**: GCC's build (CI's); Intel Macs; machines of fewer cores.
