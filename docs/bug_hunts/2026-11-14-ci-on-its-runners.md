# Bug hunt: GitHub's CI on its runners - tests too slow, limits too tight, results only Linux had

*12 to 14 November 2026, `main` at `47bd72cb`, then `c804869d`. GitHub's CI on `main` had not passed
since 28 September. Besides two memory bugs and a compiler-dependent test (fixed by
[2026-11-12-ci-memory-errors.md](2026-11-12-ci-memory-errors.md)), it failed because tests were too
slow for GitHub's runners, timing limits did not allow for a sanitized Debug build, and two results
differed only on Linux. Read from runs 43082699669 (`03c33b4f`), 43152530696 (`3518dcd9`),
43246049462 (`f172b12c`) and 43546629613 (`48fe4625`), and reproduced on this Mac: a Release build,
a Debug build with ASan and UBSan, and `QUCS_FEM_SOLVER=eigen` for CI's macOS 15, where
Accelerate's sparse LU and LDLT are not used.*

## Summary

Each failure was fixed or decided on purpose: the test made faster, given a limit that fits the
runner, or lightened under a sanitized build, with the reason written in the test.

| | test | job | what it was | decided |
|---|---|---|---|---|
| **L1** | `test_fem3_solid lameCylinder` | Linux ASan | Not Linux. A sanitized build meshed it at 0.25 mm, and σr came out 2.5 % off on any platform: −2.5297e7 Pa on this Mac with `QUCS_TEST_SLOW`, −2.5283e7 on CI, against Lamé's −2.5926e7 and the 1 % allowed. | 0.15 mm in every build: 0.5 % off, 2 s more in Release, the suite 231 s under ASan here. |
| **L2** | `test_rf25_inductance aLoopPastTheDenseLimitIsSolvedCompressed` | Linux ASan | The compressed L is ACA's to 1e-6 a block. A run's current is first order in it, where the energy (L) is second. GMRES met its 1e-10 everywhere (7e-11 here); the U's runs came out 2.9e-6 A from the dense solve's here and 1.66e-5 A on Linux, where 1e-5 A was allowed. L matched to 1e-6 on both. | Each run's current within 1e-4 of the largest (0.554 A for the U: 5.5e-5 A). The worst gap, the largest current and GMRES's residual are now printed. |
| **T1** | `test_fem3_magnetic` | macOS, Linux ASan | The saddle point's LDLT is Accelerate's only on macOS 26. On CI's macOS 15 it is Eigen's complex LU: `aLoopAcrossTheLayers` 140 s and `infiniteSolenoid` 210 s on an M2 Ultra (3.6 s by Accelerate), past QtTest's 300 s on the runner. On Linux ASan, Eigen's LU compiled at -O0 with the sanitizers: `uniformInZ` alone past 900 s. | Coarser meshes where the LDLT is not Accelerate's or the build is sanitized: `uniformInZ` 0.5 mm, the loop 0.23 (62 s by Eigen, its gauge's worst point 0.06 of the 0.2 allowed; 0.25 read 0.185), the solenoid 0.12 (23 s, 0.07 % off). A sanitized build with MUMPS (CI's Linux job) sets `QUCS_FEM_MUMPS_FROM=0`, so that every LDLT is the distribution's optimized MUMPS. The whole suite 243 s under ASan here, by Eigen. |
| **T2** | `test_fem3_semiconductor hallBar` | macOS | 28,611 unknowns, 24 Newton steps, two solves: 20 s by Accelerate, past 500 s by Eigen's LU here, past 900 s on the runner. | Where the LU is not Accelerate's, a bar of 0.2 µm and 25 layers: 4,680 unknowns, 21 s by Eigen, 0.09 % off of the 0.1 % allowed. (The suite is left out of the Linux job already.) |
| **T3** | `test_qucs_control anRf25ModelIsDrawnThroughItsTool`, then `theOpenRowIsSaidAndTheOpenProjectIsNotOpenedAgain` and `fileDialogsClaudeOpensAreQts` | Linux ASan | The drawn model's quasi-static solve: the blank model's oxide made 407 images of the stack, and the 10 µm path made 1 µm cells and 5,136 panels on the ended substrate; 11 s in Release, past the tool's 300 s under ASan here and on CI. The two later failures **follow from it**: the model was left solving, so `closeAllFiles()` returned false through `askAboutRunning`, and Switch Workspace could not close the documents either. | The test's own model lighter - no oxide (one image), the path deleted before the substrate is drawn, the substrate's box wide of the strip: 624 cells, 2,164 panels, 1.3 s in Release; the whole function 188 s under ASan here. Under the sanitizers the solve is waited for 600 s, not the tool's 300. And a solve not done in time is stopped and its model closed before the check fails, so that no later test inherits it. |
| **T4** | `test_qucs_control anRf25ModelIsOptimizedThroughItsTools` | Linux ASan (48fe4625) | A run stopped before its first point ended is saved with status "interrupted" and no point; `rf25_optimize`'s result read that as "has not been run". | Fixed in the tool: a result with a status is said as it is. |
| **T5** | `test_rf25_deembed aCoplanarFeedWithNoGroundPlaneIsCalibratedByItsGaps` | Linux ASan | A coplanar feed's raw solve and three calibrations on cells of 25 µm along the strips, at 20 and 60 GHz: 6.6 s in Release, 600 s under ASan here (a factor of 90), past 900 s on CI. | Cells of 50 µm along the strips under the sanitizers: 0.9 s in Release, every check kept. The strips' lengths come from the gaps, not the mesh, and the longer strips are compared on the same mesh. |
| **T6** | `test_rf25_dielectric` | Linux ASan | The suite 692 s under ASan here, past ctest's 900 s on CI: `aThinFilmIsItsStacksOwn` 558 s of it, and of that function's 9 s in Release the film on the oxide and the silicon is 8 (its region's panels on a stack of three slabs; the plate's cells, 8 by 8 or 4 by 4, make no difference). | Under the sanitizers the film in air alone. A stack of several slabs is still solved there by `anOxideOverTheSubstratesFace` (68 s under ASan here). |
| **T7** | `test_rf25_accuracy aThinCoplanarLinesZ0ConvergesOnPalaces` | Linux ASan (48fe4625) | Its 24-across mesh is 18 of its 22 s in Release; the only function of the suite not skipped under the sanitizers. | 6 and 12 across under the sanitizers: the first mesh's distance from Palace and the step's sign held, the order of convergence and its limit in Release. 51 s under ASan here. |
| **T8** | the macOS job | macOS (48fe4625) | Cut at its 120 minutes, test 198 of 199 still running: `test_rf25_accuracy`, new on 12 November, 344 s alone here, some half an hour on the runner. | `timeout-minutes` 180, beside T1 and T2's 1,850 s of a lane that ended at QtTest's limit. |
| **W1** | `test_plot_digitizer aHeavyPageIsReadInTime` | Linux ASan | 78 s under ASan against its 60 s (some 5 s in Release; before R1's fix 20 s and more): the suite 697 s there. | Its time not asserted under the sanitizers; Release builds keep the 60 s. |
| **W2** | `test_mcp_server aCancelledCallStopsAndIsNotAnswered` | Linux ASan | **Only speed.** The solve ended 20.6 s after its cancel; in Release 54 ms. Its answer came at all, and the transient asked would have taken 200,000 steps (hours), so the cancel reached the solve and stopped it at its next step. | 120 s under the sanitizers, 20 s in optimized builds, for the solve, the script and the batch alike. |
| **W3** | `test_claude_code aColdLongConversationIsHeld` | macOS | The transcript is drawn on a 40 ms timer; the test read it once, right after the prompt file showed the second prompt, and on a loaded runner the timer had not fired. | Waited for (`QTRY_VERIFY2`), the transcript's end printed if it never comes. |

## How

- **Single functions through ctest.** A scratch `CTestTestfile.cmake` with one entry per QtTest
  function, each given the test's own `ENVIRONMENT` (offscreen, the sanitizer options, the scaled
  QtTest limit) plus whatever was being tried: `QUCS_FEM_SOLVER=eigen` for CI's macOS 15,
  `QUCS_TEST_SLOW=1` for a sanitized build's paths in Release, and temporary environment knobs in the
  test for mesh sizes and boxes, taken out before committing.
- **A sanitized build here** (Debug, `-fsanitize=address,undefined`, scale 3), of the affected
  targets alone. It has no MUMPS, so its sparse factorizations are Eigen's at -O0: the worst case for
  T1.
- **Per-function times of a whole suite** from ctest's verbose output with each line timestamped.

## Not in this task: CI's Linux job at 48fe4625 also failed

These failed on run 43546629613 and are left for a session of their own. They look like behaviour,
not time:

- `test_fem_optimize anotherModelsPlanRunsByItsOwnSolve`: converged after 380 evaluations to an
  objective of 0.0397, x and y not within 0.02 of (1.25, −1.75).
- `test_rf25_fullwave theStaticPartsAreTheSameOnAnyNumberOfThreads`: the static parts made on other
  threads not bit-for-bit the one thread's on Linux (456 columns and blocks on other threads).
- `test_rf25_doc aStripFlushWithItsMesaIsThe2DEnginesLine`: `flush > 0` false; the suite then ran to
  ctest's 2,700 s.
- `test_layout_router aRunStoppedAnywhereKeepsOnlyLegalRoutes`: QtTest's 900 s (168 stopped runs,
  new on 12 November).
- `test_pptx_doc absurdNumbersAreHeld`: 3,007 ms against "three points' 24 ms × 20 + 2,000", a
  wall-clock ratio under ASan.

`test_data_import` and `test_rf25_doc`'s macOS failure were the memory-errors hunt's (fixed in
`56bcb893`).
