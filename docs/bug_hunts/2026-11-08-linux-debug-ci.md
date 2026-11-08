# Bug hunt: CI's Linux Debug + ASan/UBSan job, red since 7 November

*8 November 2026 - Qucs-S 27.0.8 (`119aec3`, then `de6fa0b`). CI's job "Linux · Debug + ASan/UBSan · smoke
tests" (Qt 6.10.3, GCC 11.4, Debug, ASan and UBSan with `halt_on_error=1`) passed at v27.0.6 (`86a8458`) and
failed 6 of 177 suites at v27.0.8 (run 42432357248). Each failure read from that run's log, reproduced here
where it could be, and fixed in the commit that adds this report, each fix with a test and broken in turn
([breaks.py](2026-11-08-linux-debug-ci/breaks.py): 5 of 5 caught). F8, the AppImage's crash in Application
Settings that the user found on Linux the same day, added after.*

## Summary

| | Finding | Kind | Since | Fixed |
|---|---|---|---|---|
| **F1** | `test_layout_routing`: the Route panel's destructor reached the layout's view through a document already past `~LayoutDoc` (UBSan: "downcast of address … which does not point to an object of type 'LayoutDoc'"); closing the tab while a net was being clicked crashed Release on macOS | real bug (a crash) | `1c0fbc4` (the router, 7 Nov) | `~LayoutDoc` deletes its panel first |
| **F2** | `test_plot_digitizer_app`: every function passed, then "terminate called after throwing an instance of 'std::system_error' - Operation not permitted" at exit | real bug (an abort at every exit after the digitizer's tools ran) | `cdef951` (the digitizer, 7 Nov) | the tools' PDFs freed with the application |
| **F3** | The mesher's refinement candidates, and a study's adaptive step's triangles, sorted by a key alone: each standard library put the ties in its own order, so one model meshed differently on Linux (libstdc++) and macOS (libc++) | real bug (results that depend on the platform) | `81fbb32` (10 Oct), `73bf6fe` (11 Oct) | ties broken by index |
| **F4** | The convergence check settled on the last mesh allowed, its check one past it: "not converged … the values not yet settled", which was false | real bug (a wrong message) | `063142f` (the mesh convergence check, 7 Nov) | said as it is |
| **F5** | `test_fem_adapt` (4) and `test_multiphysics_converge` (3): asserted outcomes of one mesh's numerics, macOS's; `test_fem_waves`' fine-mesh constants, after F3 | tests tuned to one platform's meshes | `063142f`, `73bf6fe` | room for the check; the rates compared at the same accuracy; the goal reference without its own check; the constants of the new meshes |
| **F6** | `test_font_settings` (4): picked Menlo, which Linux does not have; Qt's dialog gives the nearest family it has, "Sans Serif" | test assumed macOS | `72f7579` (the font panel, 7 Nov) | a family the dialog lists |
| **F7** | `test_claude_code::replyFromThePermissionCard`: a Reply moved the focus to the composer, the label lost its selection with it, and the next "Copy" was off: the click on it left the menu up for a minute | test assumed macOS's focus | `4cd382d` (4 Nov) | Copy before Reply; the helper closes a menu whose chosen item is off |
| **F8** | The Linux AppImage ended as Application Settings opened: the check of whether OpenGL can draw the viewers freed its surface before its `QRhi`, and `~QRhi` used it | real bug (a crash, Linux and the BSDs) | `d01d425d` (the viewers on Qt's RHI, 17 Oct) | the surface freed last; `test_gpu_probe` runs the check on a display |
| **N1** | Accelerate's threaded sparse factorization rounds differently from one run to the next: an adaptive run on macOS is not reproducible (two runs of one model, two meshes after 16 rounds) | finding, not fixed | - | a follow-up (below) |
| **N3** | After the merge of `f3c591c`: the uniform corner "converged" at 1e-3 with an error of 1.23e-3 in slow builds; the error said below the error made in Release | finding in main's newest change | `f3c591c` (8 Nov) | fixed by the mesh convergence session (`e1eaa8a7`, merged in `b18627cd`); `reentrantCornerRates` asserts the verdict |
| **N2** | macOS CI at v27.0.8 (run 42472828553): `test_running_jobs::anRf25ModelSolvesWithItsTabClosed`, `doc->isSolving()` false straight after `solve()` | not reproduced here | `276570d` (running simulations, 7 Nov) | the assertion now says why the solve declined |

Line 7031 of `test_claude_code`, which the task listed, is `fuzz()`'s SKIP, not a failure.

## How

- CI's log of run 42432357248 (`gh run view --log-failed`), read for each suite.
- A sanitized Debug build here (Apple clang, ASan + UBSan, `QUCS_SHARED_TEST_CORE=ON` as CI has it): every
  failing suite passed on it as it was. So each failure was of Linux, GCC, Debug or Qt 6.10.3, not of what a
  sanitizer sees on macOS.
- For the FEM suites, three more trees to tell those apart: Release with `-ffp-contract=off` (no fused
  multiply-add, as x86-64 without `-mfma`); Homebrew's GCC 16 with libstdc++ and without Accelerate (Eigen's
  solvers, as on Linux), Release and Debug. Qt here is built with libc++, so the GCC trees link two of
  QtCore's functions, whose signatures name `std::` types, from a scratch shim (`QJsonObject`'s
  initializer-list constructor and `QThread::createThreadImpl`); only `test_fem_adapt` was built that way.
- Single functions run through ctest from scratch `CTestTestfile`s with `qucs_add_test`'s environment
  (offscreen, no claude or gh), never the binaries directly.

## F1 - the Route panel outlived its document's destructor

`RouterPanel` keeps a `QPointer<LayoutDoc>` and its destructor gives the view back its mode and overlays:

```cpp
RouterPanel::~RouterPanel()
{
    if (a_doc && a_doc->view()) {
        if (a_making) a_doc->view()->setMode(LayoutView::Mode::Select);
        a_doc->view()->setOverlays({});
    }
}
```

The panel is a child of the document's splitter, so it was deleted by `QWidget::~QWidget`'s
`deleteChildren()`, after `~LayoutDoc` had run. The `QPointer` is cleared only in `~QObject`, later still, so
it was not null, and `a_doc->view()` called a member function through a pointer whose object was no longer a
`LayoutDoc` - undefined behaviour, which GCC's UBSan reports as a bad downcast (`-fsanitize=vptr`) and Apple
clang's does not. Worse than a report: `a_view` is a child too, and by creation order may already be freed
when the splitter's children go.

**Fix.** `~LayoutDoc` deletes the panel first, while the document and its view are whole. The other panels
that keep a pointer to their document (multiphysics, 3D, RF 2.5D, the digitizer, the console) have no
destructor that touches it.

**Test.** `test_layout_routing::thePanelGoesBeforeItsDocument`: a tab closed with its panel up and a net being
clicked; at the panel's `destroyed` signal, `qobject_cast<LayoutDoc*>` of the document must succeed and its
view be there. Past `~LayoutDoc` the cast fails on every platform (the object's metaobject is then
`QWidget`'s), so this catches on macOS what only GCC's UBSan saw. With the fix undone, the Release build on
macOS does not even get that far: it dies of SIGSEGV in the close, the panel's destructor setting the mode of
a view already freed - what a user closing the tab while clicking a net's terminals would have met.

## F2 - the digitizer's PDFs freed after QtPdf

`find_plots`, `digitize_plot` and `get_digitized` keep the last four PDFs read in a function-local
`static QCache<QString, Opened>` (an `Opened` holds a `QPdfDocument`). Statics are destroyed in the reverse of
their construction, and the cache was made just before the first `QPdfDocument`, whose constructor makes
QtPdf's lock, a `Q_GLOBAL_STATIC(QRecursiveMutex, pdfMutex)`. So the lock went first. `~QPdfDocument` then
takes `QPdfMutexLocker`, a `std::unique_lock<QRecursiveMutex>` of `*pdfMutex()`, which is null once destroyed:
libstdc++'s `unique_lock::lock()` throws `system_error(operation_not_permitted)` on a null mutex, in a
destructor, so `std::terminate`. libc++'s constructor calls `lock()` on the mutex without that check, and on
macOS nothing was seen.

**Fix.** The cache is made on the heap and freed by `qAddPostRoutine`, in `~QCoreApplication`, before any
static goes. With no application (never the case today) it is simply not freed.

**Test.** `test_plot_digitizer_app`'s `main` checks, after its `QApplication` is gone, that the tools hold no
PDF (`QucsControl::digitizerPdfsHeld()`); and `overStdioWithNoWindow` now checks that `qucs-s --mcp-server`
exits normally with 0 and without "terminate called" on its standard error - it had read the answers and
killed the process if it lingered, never looking at how it ended.

## F3 - the meshes of each standard library

`adaptiveStepsStillWork` asserted 1168 triangles for the shipped `adaptive_corner.qfem`'s adaptive step, and
CI's Linux made 1111. Fused multiply-add was the first suspect (arm64's clang contracts `a*b+c` by default,
x86-64's GCC cannot without `-mfma`), but a build with `-ffp-contract=off` still made 1168.

The mesher refines in passes: each triangle too large becomes a candidate, the candidates are sorted by how
much too large they are, and a point is put in for each in that order unless a neighbour's was:

```cpp
std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y) { return x.priority > y.priority; });
```

Congruent triangles - a rectangle's first mesh is full of them - have equal priorities, and `std::sort` is not
stable: libstdc++ and libc++ leave equal elements in different orders, so different neighbours are blocked and
different points go in. The adaptive step's marking (`fem_solver.cpp`, triangles sorted by their error, the
largest marked until half the error is covered) has the same form and is broken by index too, though on these
meshes it changes nothing with either library (its errors do not tie here; undone, the count stays 1107).
With the ties broken by index:

| build | before | after |
|---|---|---|
| macOS, Apple clang, libc++, Accelerate (Release, Debug) | 1168 | 1107 |
| macOS, GCC 16, libstdc++, Eigen (Release) | 1094 | 1107 |
| CI's Linux, GCC 11, libstdc++ (Debug) | 1111 | (1107 expected) |

The two macOS builds now agree to the last digit on every row of the convergence runs as well (triangles,
unknowns, values to 10 digits). The fix in `fem_adapt.cpp`'s own marking already broke ties this way. CDT
itself orders its points with a portable `nth_element` for this very reason; no unordered container is
iterated on the way to a mesh.

**Test.** `adaptiveStepsStillWork` asserts 1107, which now holds on both standard libraries; the mesher's
tie-break undone makes it fail (break 3).

## F4 - settled on the last mesh, said as not settled

The convergence check refines until each value's estimated error is within its tolerance, then makes one
more mesh with every element cut (the check against meshes that agree by chance). The L-shaped gap of the
tests (`hmax` 0.25: 104 unknowns to start, about 1.45 times more each round) settled at 1e-3 on its twelfth
mesh - the estimate 3.8e-4 - and the default limit is 12 meshes, so the check did not fit. The run then said
"Not converged: stopped at the limit on meshes" with the note "it stopped at its limit of 12 meshes, the
values not yet settled". They had settled; what was missing was the check.

**Fix.** That case is told apart: the summary says "stopped at the limit on meshes before its check", the
note that the values settled on the last mesh allowed and the check would be one more. The run is still not
converged: the check is part of the verdict, and the limit is the user's.

**Test.** `test_fem_adapt::settledOnTheLastMeshIsSaid`: the corner run once with room, then again with the
limit at the mesh before its check (the new words, none of "not settled"), and once more one mesh short of
that (the old words).

Since then main's review of the check (`f3c591c`, merged as `1b56335`) raised the default limit to 25 and
refines the corner faster, so the default now has room for the check; the message still mattered for any
limit a user sets, and the test sets its own.

## F5 - tests tuned to macOS's meshes

With F3's fix, macOS makes new meshes, and with them macOS failed what Linux had: the corner's checks
stopped at 12 meshes (`aRunAndItsPage`, `savedAndOpened`, `theTool`, `theRefinedMeshKept`), as Linux's
"stopped at the limit on meshes; 12 solves, 22453 unknowns" did. Each assertion was about one mesh's
numbers, and passed on the mesh it was written on.

- **The corner's checks** (`kCheck`, `theTool`'s call, `theRefinedMeshKept`): on the code of `de6fa0b` they
  needed 20 meshes, which this branch gave them; `f3c591c`'s default of 25 and faster refinement do that
  now, so after the merge they are as main has them, and pass.
- **`reentrantCornerRates`** asserted that the uniform route solved more than 10 times the unknowns of the
  adaptive one (1.5 in a slow build), counting every mesh each solved. That counts the meshes each needed to
  be sure, and the adaptive sequence's jitter before its asymptote costs it many: in the GCC Debug tree
  (Linux's conditions) 36,758 against 37,785. Its comment says what is meant - "the same accuracy, by
  cutting every element: many more unknowns" - so it now compares the first mesh of each within the
  tolerance of the reference: 1,084 against 9,522 at 1e-3, 9,741 against 333,985 at 1e-4 (after
  `f3c591c`, 611 against none at 1e-3 and 5,583 against 333,985 at 1e-4). The factors are the same; the
  rates and every other assertion are unchanged. A uniform run that claims convergence must now also be
  within the tolerance - else that comparison would let a wrong verdict pass (N3). On CI's Linux mesh its reference had also
  stopped after 4 meshes ("settling would take far more unknowns than allowed", from two pre-asymptotic
  rates); on F3's meshes it converges in every build here.
- **`goalOrientedRefinesForTheValue`**: its reference (goal-oriented, 1e-7) sometimes caught a "false
  agreement" - its check moved the value by 1.0e-7 - and cut every element from there, past 2e6 unknowns and,
  given room, past 4e6 without settling. Whether it does differs from run to run on macOS (N1). The
  reference stands in for the exact value, so it now runs without its own check: it converges near 212,000
  unknowns, good to about 1e-7 by that very move, ten times what the runs under test are asked (1e-6). Those
  keep their checks, and what the function asserts of them is unchanged; four runs in a row pass. (On CI's
  Linux mesh its fields-refined run had converged inside ten times the goal run's largest mesh, which the
  function then asserted it would not; `f3c591c` found the goal route no cheaper on this model and dropped
  that assertion, so after the merge only the reference's settings are this branch's.)
- **`test_fem_waves`**: the fine meshes its slow builds take as their reference changed with F3 - the
  corner's (1.132889e-11 J/m on 104,386 triangles, was 1.132892e-11 on 104,215) and the iris's (S21
  -0.29448 + 0.21104j on 34,511 triangles, was -0.29351 + 0.20825j on 34,488). The iris's moved by 3e-3
  against a tolerance of 1e-4: that check tells meshes apart rather than measuring an error. Both constants
  are the new meshes', which every standard library now makes.

- **`test_fem_photodiode_power::emittedOverAHeterojunction`**, slow builds only: the thermionic
  resistance on meshes of 1 and 0.5 nm was asserted within 3 % (it was 2.6 % on macOS's old mesh); on F3's
  meshes it is 3.3 %. **This one is loosened**, to 4 %: at 1 nm the interface's first triangles decide a
  few per cent, and 3 % had been taken from one mesh with 0.4 % to spare. Release's finer pair (0.25 and
  0.5 nm) keeps its 1 %, and passes.

All of `test_fem_adapt` and `test_multiphysics_converge` pass in Release, in the Debug + ASan/UBSan build
and (`test_fem_adapt`) in the GCC 16 Debug tree.

## F6 - the font test's Menlo

On macOS the font buttons drive the system's font panel, which the tests play (`currentFontChanged`, then
the panel closed). Elsewhere it is Qt's own `QFontDialog`, and `setCurrentFont(QFont("Menlo", 14))` on a
system without Menlo selects the nearest family the dialog lists: the button said "Sans Serif,14". Not a
bug: a dialog can only pick a font there is.

**Fix.** `aFamily()`: Menlo on macOS; elsewhere the fixed-pitch system font's family if the database lists it
and it scales, else the first scalable family. The button's text is compared by family and size, since Qt's
dialog may name a style ("Book") that the button appends.

## F7 - the permission card's Copy after Reply

`replyFromThePermissionCard`, over two lines: Reply, then Copy. Reply quotes the selection and puts the focus
in the composer, and a `QLabel` clears its selection when it loses the focus (other than to a popup or
another window). Whether it had the focus depends on the panel's window being the active one, which differs
between platforms and runs; on CI's Linux it was, Copy was disabled, and the test's helper clicked it
anyway - a click on a disabled item leaves a menu up, until the watchdog closed it 60 s later ("A QMenu left
open 60 s"). Not a bug: the label behaves as Qt's labels do.

**Fix.** Copy first, then Reply, as the single-line part above it already did. And `rightClick` closes the
menu when the item asked for is there but off, so such a test fails at once, at its own check.

## F8 - Application Settings ended the program on Linux

The user, the same day: the Linux AppImage crashes as "Application Settings..." opens. Its Viewers tab
says whether the GPU can draw (`gpu::hindrance()`, then `rhiAvailable()`), and with every viewer on the
CPU - the default - it is the first to ask: the viewers ask only for one the GPU is chosen for. On Linux
the check makes a `QRhi` on OpenGL with a fallback `QOffscreenSurface`, both locals, the surface declared
after the `QRhi`: so freed before it, and `~QRhi` makes its context current on the fallback surface to free
what it made. On macOS and Windows the check is Metal's and Direct3D's, with no surface; in the tests,
on the offscreen platform, it returns before making anything. So no test and no platform but Linux's ran
it.

Not run on Linux (no Linux machine with a display here): reproduced on macOS by the same code on its
OpenGL - a probe under ASan ended "Pure virtual function called!" (exit 134) with the source's order, and
cleanly with the surface first. The check is now `openGLFit()`, compiled wherever Qt has OpenGL, the
surface declared first. `test_gpu_probe` runs it in a process of its own on the machine's own platform
(Cocoa, Windows, X11 or Wayland; skipped on Linux without a display, as on CI's runners): with the old
order the process died on signal 11 and the test failed; with the fix it ends cleanly ("OpenGL checked on
cocoa: not fit" - macOS's default context is OpenGL 2.1, the `QRhi` made and freed all the same).
Multiphysics > Settings… asks the same check and would have ended the same way.

## N1 - adaptive runs on macOS are not reproducible

Two runs of `goalOrientedRefinesForTheValue`'s reference in one Release build agreed on every value to 12
digits through round 16, then made meshes of 41,766 and 41,802 unknowns, and one converged where the other
did not. With `VECLIB_MAXIMUM_THREADS=1` three runs were identical. So Accelerate's threaded sparse
factorization rounds differently from run to run, and the marking (a threshold on sorted indicators)
amplifies the last bit into a different mesh. Qucs-S's own threads write disjoint arrays and are not the
cause. Not fixed: making Accelerate single-threaded costs speed on every solve, and whether reproducibility
is worth it is a product decision. Options: single-threaded factorization for adaptive runs alone (BLAS
threading set around them), or rounding the indicators before they are sorted so that last-bit noise cannot
reorder them.

## N2 - macOS CI's `test_running_jobs` at v27.0.8

Run 42472828553's macOS job (the tag push of `de6fa0b`) failed one suite:
`anRf25ModelSolvesWithItsTabClosed`, `QVERIFY(doc->isSolving())` straight after `doc->solve()` (line 515). It
passes here, in Release and on ASan. `isSolving()` is false there only if `solve()` declined - no mesh, no
processed layout, or a problem with the returns - and the test did not say which. The assertion now prints
`solveProblem()` and the mesh's lines, so the next run says why. Not investigated further: it is the
running-simulations work's, not one of the failures of 7 November.

## N3 - after the merge of `f3c591c`: a uniform run converged short of its tolerance

Brought up to date with main (`a26ec63`), which merged the review of the convergence check, the uniform
route of `reentrantCornerRates` in a slow build (tolerance 1e-3; CI's Linux job runs this branch) says
"Converged ... largest estimated error 0.0931 %" while its error against the reference is 1.23e-3, and none
of its meshes is within 1e-3. In Release (1e-4) the verdict is right, but the error said (5.5e-5) is below
the error made (7.4e-5); before `f3c591c` it said 8.2e-5. The cause is `f3c591c`'s new branch for a rate
between 1.05 and 2 times the elements' order: no longer extrapolated (with room 3 when unsteady), it is
bounded at 2·|Δ|/(b^order − 1). The uniform corner's changes jitter - their ratios 0.39, 0.98 and 0.35 over
meshes of 493 to 4,620 unknowns - so one pair reads as a rate of 1.4, and the bound is 1.84 Δ where the
error left is 2.4 Δ (the true rate about 0.62). Not fixed here: the estimator is the convergence check's,
whose rules were measured on sixteen models; at the user's choice the mesh convergence session fixes it,
in its own branch, carrying this branch's `reentrantCornerRates` hunk unchanged. Before that fix,
`reentrantCornerRates` failed in slow builds - on the new assertion that a converged run is within its
tolerance, and only there (clang's ASan build and the GCC 16 Debug tree gave the same numbers: said
9.32e-4, made 1.23e-3). Fixed in `e1eaa8a7` (merged in `b18627cd`), which this branch was brought up to
date with before its commit.

## Checked

On this branch brought up to date with local main (`a26ec63`, which has `f3c591c`'s review of the
convergence check, the optimization recheck, the git fixes and the digitizer's rework):

- Release, the whole suite through ctest: 181 of 181.
- Debug + ASan/UBSan (Apple clang), the suites touched and every FEM and multiphysics suite: all pass but
  `test_fem_adapt::reentrantCornerRates`, on N3 alone. (On the code before the merge, all 58 FEM-side
  suites: `test_fem_photodiode_power`'s slow tolerance, F5, and `test_fem_quantum_2` past QtTest's 300 s
  with eight sanitized suites at once; alone it passes, 975 s.)
- GCC 16 Debug with libstdc++ (CI's Linux conditions for the mesh and the numerics): `test_fem_adapt`, the
  same as the ASan build, N3 alone.
- breaks.py: 4 of 4 caught. The four lints in `scripts/ci/`: green (158 tools).

Then brought up to date with main again (`29677138`: the mesh convergence session's estimator fix
`e1eaa8a7`, the digitizer's recheck, the chat's note fix, the pen markers, the presentation tab, the
jobs window's columns; no conflict), the nine suites this commit changes, through ctest:

- Release: 9 of 9.
- Debug + ASan/UBSan (Apple clang): 9 of 9 - `reentrantCornerRates` among them, N3 fixed.
- The four lints: green (158 tools).

F8, after: `test_gpu_probe` in Release and on ASan, passing; breaks.py's fifth break caught.

## Not checked

- CI itself: nothing was pushed. The Linux job takes about 5.5 hours; the GCC tree here stands in for its
  standard library and arithmetic, not for Qt 6.10.3, x86-64's `libm` or GCC's UBSan.
- `test_multiphysics_converge` under GCC: it needs the application's libraries, which the GCC tree cannot
  link against Homebrew's Qt; it passes in Release and on ASan here.
- The font test's non-macOS branch: compiled and reasoned from Qt's `QFontDialog`, not run.
- Windows.
- F8 on Linux itself: the AppImage was not rebuilt or run; the fix is of the cause reproduced on macOS's
  OpenGL.

## Follow-ups

- N1, if reproducible adaptive runs are wanted on macOS.
- N2, from the next macOS CI run's message.
