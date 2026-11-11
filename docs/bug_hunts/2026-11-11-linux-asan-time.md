# Bug hunt: CI's sanitized Linux job past its 355 minutes

*11 November 2026, main at `301cbdec`. CI's job "Linux · Debug + ASan/UBSan · smoke tests" (`linux-asan` in
`.github/workflows/ci.yml`) has not finished on main since before 10 November: cancelled by its own limit,
or by the next push. Where its time goes, why `test_fem_modulator` aborts, the ways to make it fit and what
each costs in coverage, and which one the user took. The numbers come from three runs' logs and from the
trial branches of 5 November (`ci/asan-speed-trial`, `ci/asan-canary`), which ran on 10 November. Scripts:
[2026-11-11-linux-asan-time/](2026-11-11-linux-asan-time/).*

## Summary

| | Finding |
|---|---|
| **T1** | The unit tests use the runner's four processors from start to finish: on all three runs, 4.00 tests run at once in every ten-minute window but the last. The job is limited by how much work there is, not by the order the tests run in. Only less work per test, or more machines, will shorten it. |
| **T2** | The work is the multiphysics engine's: the 62 `test_fem*`, `test_multiphysics*` and `test_rf25*` suites are 56,114 of the 60,770 test-seconds on the Xeon 8573C run (92 %), the 38 `test_fem*` 46,283 of them (76 %). The 10 slowest suites are half of it, the 20 slowest 73 %. |
| **T3** | Its length depends on the processor the job is given: the same tests took 39,968 test-seconds on an EPYC 9V45, 60,770 on a Xeon 8573C and about 71,300 on an EPYC 7763 (1.78 times the 9V45). |
| **T4** | `test_fem3_semiconductor` (`79b64a81`) adds about 9,900 test-seconds on a 7763, by the 2D suite's ratio (its 81 s on an M2 Ultra, 4,520 s on the 7763). Its Release times alone on that machine: `modelsAgainst2D` 64.8 s, `impactDrivingsAgainst2D` 47.0, `impactAgainst2D` 20.2, `heterojunctionsAgainst2D` 12.0, the other 25 functions 33 s together. Its comparisons with the 2D solver are most of it. `301cbdec` gave it a ctest limit of 2,400 s and QtTest 600 s a function times the scale (7,200 s and 1,800 s on this job), short of the ~9,900 s and ~3,600 s it would need at `-O0` on a 7763. On main as it is, the job needs about 435 minutes on a 7763 and 378 on an 8573C, against its 355. Only a 9V45 still fits (about 253). |
| **T5** | `test_fem_modulator`'s "Subprocess aborted" is QtTest's limit on one function, not a crash: `wellsAbsorptionOfTheField` reached 900,003 ms, the 300 s default times the job's scale of 3, and QtTest aborts (`QFATAL … Test function timed out`, SIGABRT). It runs five mode solves with the wells' absorption tables. On the 7763 it ran beside three other multiphysics suites (the quantum test's second part, the nitrides', the laser's) and the other eleven functions took 725 s. On the 9V45 and the 8573C the suite passed (1,679 and 2,468 s), so the function is close to the limit, not far past it. On macOS it has 900 s and runs alone (`RESOURCE_LOCK fem_memory`); on Linux it has neither. `test_fem_crystal`'s `channellingOnAxisGoesDeeper` is cut the same way, at 900 s, on all three runs. |
| **T6** | The trial of 5 November measured the same build at other optimization levels (its code then 162 suites): the unit tests took **3,557 s with `-O1 -g1`** and 6,216 s with `-Og -g1`, both on an EPYC 9V74, against about 10,000 for today's 189 suites at `-O0` on a 9V45. On one machine (an EPYC 7763) with the same code, `test_fem_physics` took 63 s at `-O0` and 17 s at `-O1`: 3.7 times faster. |
| **T7** | Its canary (`scripts/ci/canary.cpp` on `ci/asan-canary`: 48 kinds of defect, each compiled with the job's GCC at each level) finds the same defects at `-O1` as at `-O0`, with three exceptions. (1) A defect whose result is never used is gone, because the compiler deletes dead code: a `malloc` freed twice with nothing between, an overflow into a variable never read. (2) Some reports change kind but still fail: a heap or stack read past the end is reported by UBSan instead of ASan. (3) Ubuntu's default `_FORTIFY_SOURCE` stops two `strcpy` overflows with an abort before ASan sees them; `-U_FORTIFY_SOURCE` gives ASan's report back. |
| **T8** | ASan's `detect_stack_use_after_return`, on by default with Ubuntu's GCC 13, costs about 30 % of a FEM suite: `test_fem_physics` 63 s with it and 43 s without (7763), 45 and 32 s (9V74). |
| **T9** | The job fails on tests as well as on time. On every run that finished: `test_rf25_stack` (ASan: heap-use-after-free at `test_rf25_stack.cpp:653`, a `const Layer*` into the `Stack` that `back.stack()` returned by value, a temporary gone at the end of line 651); `test_multiphysics3d_doc` (its ctest limit of 900 s); `test_gui_monkey` ("no wheel turn moved a PDF"); `test_fem_crystal` (T5). On the 7763 and the 8573C, `test_rf25_returns` (its 900 s); on the 7763, `test_fem_engine` and `test_fem_sparse` (each asserts a step under 60 s of wall clock; the engine's took 97 s); on the 8573C, `test_theme_switch`. |
| **T10** | With `cancel-in-progress: true` for every ref, each push to main cancels the running job. On 10 and 11 November main was pushed every 1.5 to 2.5 hours, so a job of 5 to 6 hours finishes only on tags and on the one quiet night. |

## Where the time goes

Three runs of the job, each with the unit tests run four at a time (`ctest --parallel 4`):

| run | commit | processor | build | unit tests | test-seconds | outcome |
|---|---|---|---|---|---|---|
| 42956129935 (main) | `1ad5bbba` | EPYC 7763 | 71 min | 279 min, then the limit | 58,362 for 153 of 189 suites | cancelled at 355 min |
| 42956132899 (tag v27.1.2) | `1ad5bbba` | EPYC 9V45 | 47 min | 167 min | 39,968 | 4 failed |
| 42921522006 (tag v27.1.1) | `6c99e4d3` | Xeon 8573C | 71 min | 256 min | 60,770 | 6 failed |

The ten slowest suites, in seconds:

| suite | 7763 | 9V45 | 8573C |
|---|---|---|---|
| `test_fem_devices` | (still running) | 4,024 | 6,256 |
| `test_fem_semiconductor` | 4,520 | 2,264 | 3,710 |
| `test_fem_waves` | 3,515 | 1,786 | 2,844 |
| `test_fem_quantum_2` | 3,317 | 1,634 | 2,729 |
| `test_fem_optoelectronics` | 3,212 | 1,672 | 2,481 |
| `test_fem_photodiode_power` | 3,105 | 1,551 | 2,548 |
| `test_multiphysics_doc` | (still running) | 1,965 | 2,976 |
| `test_fem_laser` | 2,836 | 1,619 | 2,427 |
| `test_fem_exciton` | 2,599 | 1,266 | 2,146 |
| `test_fem_quantum` | 2,471 | 1,194 | 2,035 |

`test_fem_devices` starts 3 hours in and runs to the end (7,300 s expected on a 7763). The tail is short all
the same: the other three lanes are full until the last ten minutes (T1).

**Projection for main (`301cbdec`), `-O0` as now.** The 7763's test-seconds are 1.173 times the 8573C's on the
suites both finished, which puts the 7763's full set at about 71,300. Add 9,900 for `test_fem3_semiconductor`
(T4), then divide by four lanes. With setup (4 min), the build, and the smoke tests and fuzzing (some 15 to 20
min at `-O0`):

| processor | unit tests | job | fits in 355? |
|---|---|---|---|
| EPYC 7763 | 338 min | ~435 min | no |
| Xeon 8573C | 288 min | ~378 min | no |
| EPYC 9V45 | 190 min | ~253 min | yes |

The `-O0` job can only pass when it is given a 9V45, so the limits can't fix it: 355 is already five minutes
under the hosted runners' 360.

## T5: test_fem_modulator

```
QINFO  : TestFemModulator::wellsAbsorptionOfTheField() the mode: loss 1641.5 per m at 50 kV/cm, 1204.5 at 0; …
         wellsAbsorptionOfTheField function time: 900003ms, total time: 1625280ms
QFATAL : TestFemModulator::wellsAbsorptionOfTheField() Test function timed out
```

`QTEST_FUNCTION_TIMEOUT` is 900,000 ms on that job, from the scale block in `qucs/tests/CMakeLists.txt`. On
macOS the modulator is one of the five memory-heavy suites (`test_fem_laser`, `test_fem_resonator`,
`test_fem_quantum`, `test_fem_modulator`, `test_fem_optoelectronics`): they have at least 900 s a function
there, and `RESOURCE_LOCK fem_memory` runs them one at a time. Neither applies on Linux. There, on the 7763,
the function ran beside the quantum test's second part, the nitrides' and the laser's, the three heaviest
suites of the hour. The function itself has not changed since 22 October (`2f912dea`). Its last part is
already skipped under the sanitizers (`slowBuild()`). It does five mode solves of the modulator's stack with
the quantum wells' absorption tables: the wells' spectrum at 0 and 50 kV/cm, the mode at both, and the
photon energy rounded to ten digits.

## The options

Each measured or projected for the slowest processor CI gives the job, an EPYC 7763. "Coverage" means what the
job would stop seeing.

**A. Build at `-O1 -g1 -U_FORTIFY_SOURCE`** (one line in the configure step: `CMAKE_CXX_FLAGS_DEBUG`).
Same tests, same sanitizers, same limits.
- Time. The unit tests 2.7 to 3.7 times faster (T6): 91 to 125 min on a 7763. The build 6 to 17 min longer.
  The job about 190 to 230 min on a 7763, some 140 on a 9V45: two hours of room.
- Coverage. What the canary loses (T7): defects in code whose result is never used. A read past the end
  reported by UBSan instead of ASan, still a failure. With `-U_FORTIFY_SOURCE`, ASan's own report of a
  `strcpy` overflow, not glibc's abort. Debug assertions stay on (no `NDEBUG`). Stack traces keep their files
  and lines (`-g1` has the line tables); a debugger loses local variables, which CI never uses.
- Numerics. x86-64 without `-mfma` contracts nothing at either level, and neither uses `-ffast-math`. The
  trial's `-O1` run passed all 162 suites but one, `test_claude_code`'s Reply and Copy, fixed on 8 November
  (F7 of [2026-11-08-linux-debug-ci.md](2026-11-08-linux-debug-ci.md)).
- Side effects. The suites cut by their limits on the slow runners would likely pass: `test_multiphysics3d_doc`
  and `test_rf25_returns` ran 7.4 times faster in the trial, `test_fem_modulator` 4.2. So would the two 60 s
  wall-clock assertions (T9).

**B. Split the unit tests across parallel jobs.** One job builds and uploads the build (2.1 to 2.6 GB with
zstd, packed in 40 to 60 s). Two or three jobs download it (1.2 min), unpack it (1.4 min) and each run a
share of the suites. The trial's "Hand over" jobs did this.
- Time. Two shards: about 80 + 190 = 270 min on a 7763, 85 min of room. Three: about 215.
- Coverage. None lost; stays at `-O0`.
- Cost. A public repository's standard runners cost nothing, but this takes three or four runners a push in
  place of one. The shares need a list kept by hand (ctest has no sharding by cost), and two-thirds of the
  room is gone again with the next few heavy suites. The build's 2.5 GB artifact on every push.

**C. Run the heaviest suites in a reduced mode under the sanitizers.** This is already done: 45 test files
take a shorter path when `slowBuild()` sees ASan, and the times above are with it. Going further means
cutting each suite's solves one by one, so the engine's numerics get less of the sanitizers' eyes.
`test_fem3_semiconductor` has no `slowBuild()` yet. Giving it the 2D suite's (its comparisons with the 2D
solver on smaller models) might save a few thousand of its 9,900 s. Even all 9,900 saved leaves the 7763 at
about 385 min.

**D. Move the heaviest suites to a scheduled run.** Each push runs everything but the 20 slowest suites (73
% of the time): about 90 min of tests, a job of about 180. A nightly job runs those 20. At `-O0` that nightly
job would need about 250 min of tests plus the build on a 7763, near the limit again. A push that breaks the
engine under a sanitizer is seen the next morning, not on that push.

**E. `detect_stack_use_after_return=0` for the engine's suites only** (their ctest environment). About 30 %
off those suites (T8). They are 76 % of the test time, so this saves some 23 %: the 7763 at about 355 min,
at the limit on its own.
Coverage: no check for a pointer to a local used after its function returns, in the engine's tests (the
application's tests keep it).

**F. ccache.** A rebuild from a full cache took 230 s against 74 min (trial). But the repository's Actions
cache is full (10.9 GB of its 10), the Qt and MSYS2 caches would be evicted, and a change to a widely included
header recompiles most of it. It shortens only the build.

**And, beside any of them: don't cancel main's running job.** `cancel-in-progress: ${{ github.ref !=
'refs/heads/main' }}` lets a job on main finish. GitHub then keeps one run waiting and drops older waiting
ones, so main gets a result on every job's length, not only on quiet nights (T10).

## Recommendation

A, with the room it gives used for nothing else, plus not cancelling main's running job. A keeps every suite
and every sanitizer on every push. The trial measured it on this job and the canary measured what it misses,
and it fixes T5 and three of T9's slow-runner failures as a side effect. If the job grows past A's room, B
comes next on top of it, not instead of it.

## Decision

The user: "ok, maybe just exclude the problematic tests from CI for now." The sanitized Linux job leaves out
17 of its 193 tests (`ctest -E` in its unit-test step). The macOS job still runs all of them, in Release:

| left out | why | seconds on a 7763 |
|---|---|---|
| `test_fem3_semiconductor` | over 50 min (estimated) | ~9,900 |
| `test_fem_devices` | over 50 min | ~7,340 |
| `test_fem_semiconductor` | over 50 min | 4,520 |
| `test_fem_waves` | over 50 min | 3,515 |
| `test_multiphysics_doc` | over 50 min | ~3,490 |
| `test_fem_quantum_2` | over 50 min | 3,317 |
| `test_fem_optoelectronics` | over 50 min | 3,212 |
| `test_fem_photodiode_power` | over 50 min | 3,105 |
| `test_rf25_stack` | ASan: heap-use-after-free in the test (T9) | 12 |
| `test_gui_monkey` | fails on every run (T9) | 16 |
| `test_theme_switch` | failed on the 8573C | ~49 |
| `test_multiphysics3d_doc` | its 900 s limit, every run | 901 |
| `test_fem_crystal` | QtTest's 900 s a function, every run (T5) | 941 |
| `test_rf25_returns` | its 900 s limit on the 7763 and the 8573C | 900 |
| `test_fem_modulator` | QtTest's 900 s a function on the 7763 (T5) | ~2,900 |
| `test_fem_engine` | a 60 s wall-clock check, 97 s on the 7763 | 494 |
| `test_fem_sparse` | a 60 s wall-clock check on the 7763 | 542 |

The rest is about 37,300 test-seconds on a 7763: 155 minutes on four lanes, a job of about 250 minutes, some
100 under its limit (on a 9V45 about 150). `test_fem_quantum`, the quantum test's first part, stays.

What the job no longer sees, until each suite comes back: the sanitizers on the semiconductor drift-diffusion
solvers in 2D and 3D, the devices, the waveguide modes, the optoelectronics, the photodiode, the modulator
and the quantum test's second part. Also the engine's own suites (`test_fem_engine`, `test_fem_sparse`), left
out over one timing check each, and one real ASan finding in `test_rf25_stack`. Option A of the list above
would bring the slow eight back within the limit. The failing nine each need their own fix: a one-line fix
for `test_rf25_stack`, room or a slow-build path for the ones cut by limits, and the timing checks skipped
under the sanitizers.
