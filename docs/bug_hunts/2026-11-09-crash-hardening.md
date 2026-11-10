# Bug hunt: crash hardening - the program and its modules fuzzed

*9-10 November 2026 - Qucs-S 27.1.1 (`de547cc9`), a Release build and an ASan/UBSan (Debug) build of
the worktree `crash-hardening-58de8d`, macOS 27.2, Qt 6.12. The user: "Now I'd like to focus on crash
hardening for qucs GUI and its modules. Run some fuzz and robustness checks and see if you can spot any
crashes." Found and documented here; then all fixed (the user's "Fix them all"), each with a test that
fails on the code before it (the table's last column).*

## Summary

| | Finding | Kind | Fixed |
|---|---|---|---|
| **C1** | A boolean of 2D geometry whose numbers do not fit Clipper's integers throws `ClipperLib::clipperException`, which nothing catches: Qucs-S aborts, every tab with it. A 20 mm die with a hole, drawn in nm, does it on opening; so does a ring of an Extrude's sketch, and a GDS file with a damaged `UNITS` record cut by a cross-section model | crash (abort) | yes: the scale from the boolean's own inputs too; coordinates out of range refused; Clipper's exception caught in each place - `test_fem_engine`, `test_fem3_geometry`, `test_fem_layout` |
| **C2** | An atomic `batch` (or `run_script`) that fails because the user opened a dialog puts the schematic back under that dialog: a wire label's dialog then reads and writes the freed label (ASan: heap-use-after-free in `LabelDialog::slotOk`; Release: silent heap corruption) | crash (use after free) | yes: not put back under a dialog, said with the undo that takes it back - `test_qucs_control` |
| **C3** | A 3D model's mesh with a tiny `hmax` (`1e-9`, a typo for `1e-3`) allocates until `std::bad_alloc`, uncaught in the mesh thread: Qucs-S aborts after 8 s | crash (abort) | yes: a mesh past 20 million prisms refused before it is made; `bad_alloc` caught - `test_fem3_mesh` |
| **H1** | An SVG with a `<use>` bomb in its `<defs>` that nothing at the root draws passes the SVG guard (`svgguard.cpp`), and QtSvg walks every `<use>` it read: the window freezes for minutes on opening it as a picture, in a deck, or in Markdown | hang (GUI thread) | yes: every scope that holds a `<use>` counted - `test_pptx_doc` |
| **H2** | Closing a 3D model while its geometry builds blocks forever when the build does not end: `~Multiphysics3DDoc` waits for a thread it cannot stop. An Array of 2 by 2,147,483,647 copies (its limit of 10,000 checked in ints, where that is -2) is one such build | hang (GUI thread, for ever) | yes: the Array's count checked as numbers (2 by 2147483647 was -2 as ints); a closed tab waits 2 s, then leaves the job to end - `test_multiphysics3d_doc` |
| **H3** | A 2D rectangle `1e308` units tall sends the union's Delaunay triangulation (CDT) into a loop, on the GUI thread at opening | hang (GUI thread) | yes: a geometry past 1e60 refused before it is triangulated - `test_fem_engine` |
| **H4** | Markdown of four lines - a quote holding `<summary>`, then `<b>b</b>` and a run of 64 `}` - loops without end in `markdown::breakLongRuns`: opening it as a document freezes the window. The Claude panel runs the same function over the transcript | hang (GUI thread) | yes: the fragments' places kept, not the fragments (stale after an edit) - `test_markdown_doc` |
| **Q1** | qucsator_rf: an S-parameter file part (`SPfile`) with one node and a Touchstone file that is missing or unreadable dereferences NULL (`spfile::createIndex`, SIGSEGV) | crash (bundled simulator) | yes: no ports until a file is read; a file not read said and left - `test_qucsator_robustness` (new) |
| U1 | A schematic text painting at angle -2147483648: `-angle` overflows an `int` (UBSan, `graphictext.cpp:268`) | undefined behaviour | yes: the angle within a turn at loading - `test_loader` |
| U2 | qucs-spowercombining `--json` with `f` 1e300: `num2str` casts infinity to `int` (UBSan, `qucspowercombiningtool.cpp:2098`) | undefined behaviour | yes: `num2str` of a number not finite as it is - `test_wishlist_tools` |
| N1 | qucsator_rf's `assert()` equation function calls `abort()` when false (481 of 38,680 runs): by design, for its test netlists | note | - |
| N2 | qucsator_rf: a transient of a stiff circuit with a 1e-16 s minimum step runs for minutes (90 runs past 20 s) - slow, not hung | note | - |
| N3 | 2D objects on top of each other cost the union about n²: 400 identical circles open in 12 s, the fuzzer's 2,000 in minutes, on the GUI thread | slow | - |
| N4 | `test_claude_fuzz` seed 8, step 623: "drawn whole, the selection went from `ssssssssss R1 ▍` to `ssssssssss R1`" - the panel's own invariant (a reply's cursor mark in a selection), not a crash | panel invariant | - |

C1, C2, C3 and Q1 ended the program; H1 to H4 left it frozen with no way out but to force it to
quit. All but Q1 are in Qucs-S's own code; Q1 is upstream qucsator's. Each new test was run on the code
before its fix, the fixes' sources put back to `de547cc9`'s: C1's three and Q1's ended in an abort or a
signal, H3's and H4's never ended (stopped at 90 and 60 s), H1's and C3's failed; C2's, U1's and U2's
failed on ASan/UBSan (Release does not see them). H2's endless Array and C3's mesh past its budget were
not run on the old code, which would have filled the machine's memory - they were seen in the fuzzing. With
the fixes: Release's 189 suites through ctest (the four lints among them) and, on ASan/UBSan, the 14
suites the fixes touch (`test_fem_engine`, `test_fem3_geometry`, `test_fem3_mesh`, `test_fem_layout`,
`test_fem_process`, `test_markdown_doc`, `test_pptx_doc`, `test_image_doc`, `test_multiphysics3d_doc`,
`test_qucs_control`, `test_mcp_server`, `test_loader`, `test_wishlist_tools`, `test_qucsator_robustness`)
pass; `repro_c2.py` on ASan survives with no report.

## How it was fuzzed

Four new fuzzers, in this folder, and the suites' own:

| Run | Build | Time | Amount | Events |
|---|---|---|---|---|
| `fuzz_docs.py`: damaged files of 22 kinds opened in the real program (`qucs-s --mcp-server`, offscreen), read by their tools, drawn, closed (now and then at once, while loading); a data file imported beside a schematic, a stack file into an RF 2.5D model, a `.lyp` beside a layout | Release, 3 seeds | 20 min each | 35,223 files | 28 events: C1 ×23, H1 ×2, H2, N3 ×2 |
| the same without `.qfem`/`.qfem3d` (C1 hid the rest) | ASan/UBSan, 2 seeds | 25 min each | 22,854 files | H1 ×2, U1 |
| the same, `.qfem`/`.qfem3d` alone, C1's aborts set aside | Release | 20 min | 15,549 files | 13 events: C1 ×10, C3, H2, H3 |
| `fuzz_tools.py`: the 161 tools with arguments from their schemas (wrong types, missing, huge, hostile), a document of each kind open, documents closed and reopened between calls, calls pipelined behind slow ones, arguments nested 2,000 deep | ASan (2 seeds), Release (1) | 25-35 min each | 3,823 calls | none |
| `fuzz_viewers.py`: a monkey for the viewers `test_gui_monkey` does not walk (data display, 2D and 3D models, RF 2.5D, layouts, PDF, decks, Markdown, pictures, spreadsheets): raw clicks, drags and keys on each tab and its bar through `send_input`, the bar's controls through `set_ui`, menus through `context_menu`, `show_*` with odd arguments, builds and solves closed under their tab, dialogs answered | ASan, 2 seeds | 25 min each | 5,275 steps | none |
| `fuzz_tooljson.py`: the `--json` mode of qucs-sfilter, qucs-sactivefilter, qucs-sattenuator, qucs-spowercombining, qucs-strans and rxcalc, specs from the tools' schemas and the keys each reads | ASan | 25 min | 1,371 specs | U2 |
| `fuzz_cli.py`: qucsator_rf on mutated netlists (the 12 of its tests, 28 made from the examples) and their Touchstone files | Release, 3 jobs | 20 min | 38,680 runs | Q1, N1, N2 |
| The suites' fuzzers through ctest, seed 7: `QUCS_EXPR_FUZZ`, `QUCS_DATASET_FUZZ`, `QUCS_IMPORT_FUZZ`, `QUCS_LAYOUT_FUZZ`, `QUCS_LUMERICAL_FUZZ`, `QUCS_MARKDOWN_FUZZ`, `QUCS_PROFILE_FUZZ`, `QUCS_ROUTER_FUZZ`, `QUCS_CHAT_FUZZ`, `test_claude_fuzz` and `test_git_fuzz` (`QUCS_FUZZ_SECONDS`), `test_gui_monkey` (3,000 steps) | ASan | 5 min each (QtTest's function limit) | | C2 (the monkey, step 429) |
| The same, seed 8, the limit raised to their 10 minutes (`QUCS_TEST_TIMEOUT_SCALE=8` in the build tree) | ASan | 10 min each | | H4 (`test_markdown_doc`, past its 40 min), N4 (`test_claude_fuzz`); the other ten passed |

`mutate.py` is the mutations (flips, cuts, splices, repeats; numbers made 2^31, 1e308, NaN, empty; lines
dropped, repeated, swapped; XML elements unclosed, renamed, nested 5,000 deep; attribute values), an
archive's members (`.pptx`, `.xlsx`, `.zip`) mutated inside it. `mcp.py` is the client of the server, `ips.py`
reads macOS's crash reports (the stacks of Release's aborts).

## C1. Clipper's exception ends the program

```
libc++abi: terminating due to uncaught exception of type ClipperLib::clipperException: Coordinate outside allowed range
 8 libc++abi   __cxa_rethrow
 9 qucs-s      ClipperLib::ClipperBase::AddPath
10 qucs-s      ClipperLib::ClipperBase::AddPaths
11 qucs-s      qucs_s::fem::Builder::boolean(Node const&)             fem_geometry.cpp:1213
12 qucs-s      qucs_s::fem::Builder::feature
13 qucs-s      qucs_s::fem::Builder::build
14 qucs-s      qucs_s::fem::buildGeometry
15 qucs-s      MultiphysicsDoc::buildNow(QString*)
16 qucs-s      MultiphysicsDoc::load()
17 qucs-s      QucsApp::gotoPage                                      opening it
```

Clipper works in 64-bit integers and refuses a coordinate past ±4.6·10^18 by throwing. `boolean()` turns
the inputs' coordinates into integers with `clipperScale()`, about 10^12 steps across the largest
coordinate of the geometry - but it measures `a_objects`, which by then no longer hold the boolean's own
inputs and tools: `inputs(f, ..., remove)` has taken them out. With nothing else left (a die and its hole,
the circles of a ring's sketch) the extent stays at its floor of 1 and the scale is 2^39: any coordinate
past 8.4·10^6 units overflows. In a model drawn in nm that is 8.4 mm.

- [c1_die_in_nm.qfem](2026-11-09-crash-hardening/c1_die_in_nm.qfem): a 20 mm square with a 2 mm hole,
  in nm - opening it aborts Qucs-S. 10 mm (corners at ±5·10^6) still opens.
- `extruded_ring.qfem3d` with `r` 9·10^6 aborts at its build (5·10^6 builds): the Extrude's sketch is a 2D
  geometry built by the same `Builder`.
- `gapped_core_inductor.qfem` with the window 2147483648 wide: the air (240) is left, so it takes a
  coordinate of 10^9.
- [c1_gds_units.gds](2026-11-09-crash-hardening/c1_gds_units.gds): `thermo_optic_heater.gds` with the
  `UNITS` record's metres per database unit made 7·10^75. Beside the example `heater_crosssection.qfem`
  (a Layout Cross-Section of it), opening the model aborts: `fem_import.cpp` scales the layout's
  coordinates by a fixed 10^4 with no check. (The layout viewer reads the same file without harm.)
- A non-finite coordinate - `1e308` times a scale is infinity, and infinity times a scale of 0 is NaN -
  reaches `llround`, whose result is then out of range too (`fem3_geometry.cpp`'s `clipped()`).

Nothing catches a `clipperException` anywhere: the same holds for `fem_process.cpp` (process flows) and
`layoutrouter/router_verify.cpp`, where no input of the fuzzers reached it.

**Fixed**: `clipperScale()` measures the boolean's inputs and tools with the objects left, from their
edges' ends; a coordinate not finite (NaN included, which `std::max` let through) or past 1e300 fails the
feature ("a coordinate is out of range"). Clipper's calls are inside `catch (const
ClipperLib::clipperException&)` in the 2D boolean, the 3D plan's clip to its window (which refuses a
coordinate not finite first), the GDS import (a coordinate past 1e14 µm refused: "its units are damaged,
or it is not whole") and `runProcess()` (a failed run). `router_verify.cpp` works in the layout's own
64-bit integers, read from 32-bit GDS coordinates, far inside Clipper's range: left as it is. Tests:
`test_fem_engine`'s `booleansOfHugeAndEndlessCoordinates` (the die in nm built, its area right; squares
2^31 wide united and intersected; 1e308, `1e308*10` and -1e308 refused), `test_fem3_geometry`'s
`hugeSketches` (the ring of an Extrude's sketch at r 9e6 and 2^31 built, 1e308 refused) and
`test_fem_layout`'s `damagedUnits` (the heater layout with C1's `UNITS` bytes: an error, no layers).

## C2. An atomic batch put back under the user's dialog

`test_gui_monkey`, seed 7, step 429 (about one run in two; the monkey's fake Claude, `monkey_claude.sh`,
sends batches): ASan's heap-use-after-free in `LabelDialog::slotOk` (`labeldialog.cpp:132`,
`pLabel->Name`). With the allocation stacks kept (`malloc_context_size=30`, in the build tree's CTest
file for the run) it was freed by:

```
 1 WireLabel::~WireLabel()
 6 Conductor::~Conductor()
 7 Node::~Node()
10 Schematic::deleteAllElements()                    schematic.cpp:166
11 Schematic::rebuild(QString*)                      schematic_file.cpp:1704
12 Schematic::restoreAll(...)                        schematic_file.cpp:788
14 BatchRun::finish()                                qucscontrol.cpp:12695
15 BatchRun::next()
```

A dialog that waits for an answer holds what it edits, and since 19 October `QucsControl::call()` refuses
every tool but those that look while one waits (`whileADialogWaits`). An atomic batch that began before
the dialog opened meets it at its next call, which is refused - so the batch failed, and `finish()` put
every schematic back as it was before the batch: `restoreAll()` rebuilds it, every wire, node and label
made anew, under the dialog. Its OK then reads the freed label, and `MouseActions::editLabel()` writes to
it (`pl->setName`, `pl->initValue`). In Release nothing stops at once: the heap is corrupted, to fail
later or not at all.

[repro_c2.py](2026-11-09-crash-hardening/repro_c2.py) does it every time with tools alone: a wire, its
label, then `batch` atomic of `add_component`, `send_input` double-clicking the label (its dialog opens),
`add_component` (refused); then `set_dialog` OK. On ASan the server dies with the report above.

`run_script`'s atomic rollback (`qucscontrol_script.cpp:231`) is the same.

**Fixed**: an atomic batch that fails while a dialog is open is not put back: its answer says "Atomic:
not undone - a dialog is open (its title), and putting a schematic back under it is not safe", and what
stays, with the undo that takes it back once the dialog is closed. (A put-back put off until the dialog
closed would also throw away what the user answered in it.) `run_script`'s atomic rollback the same,
in its "put back" field. A test: `test_qucs_control`'s `anAtomicBatchIsNotPutBackUnderADialog` -
`repro_c2.py`'s calls, the label's dialog answered with "other" once the batch has answered; the batch's
resistor stays and the label is "other". On ASan the old code dies in `LabelDialog::slotOk`.

## C3. A 3D mesh without a budget

`msm_photodiode.qfem3d` with its domain `size` feature's `hmax` `0.1` changed to `1e-9` (µm): `fem_build`
mesh aborts the program after 8 s with `std::bad_alloc` from the mesh thread (`1e-320` builds on, at 80 %,
for as long as it is let). No code of Qucs-S catches `std::bad_alloc`; the 2D meshes, the dopants' grid
and the 2D counts have budgets since the hunts of October, the 3D plan mesh has none.

**Fixed**: the cause is the divisions in z: `sweptDivisions()` asked 1e6 for 1e-9 across 1 µm, and
`int(ceil(total))` past an int's range for smaller sizes, then the prisms, the plan's triangles times the
heights, were made. Now a slab's divisions by size are at most 100,000 (`MaxSweptDivisions`, a
Distribution's count's limit), and the prisms are counted before anything is made: past 20 million
(`MaxPrisms`), or the plan's nodes times the heights past 200 million, the mesh is refused, said with its
count ("a larger size, or fewer divisions across the heights"). `buildMesh3()` turns `std::bad_alloc` into
a failed mesh, and the 3D tab's job thread catches it too (the geometry, mesh or solve it was at failed,
said). (The solve's memory estimate needs the mesh; a count of prisms does not.) Tests: `test_fem3_mesh`'s
`divisionsClamped` (1e-9, 1e-15, 1e-300 across 1: 99,999 cuts) and `hugeMeshesRefused` (a box meshed at
0.02 with 100,000 divisions in z: refused in milliseconds).

## H1. An SVG bomb the guard does not count

`svgguard.cpp`'s `nodes()` counts what the document draws, from its root through every `<use>`. QtSvg's
reader does more: as it reads, it walks every `<use>` element it has read, also those in `<defs>` that
nothing draws. [h1_defs_bomb.svg](2026-11-09-crash-hardening/h1_defs_bomb.svg) is 15 levels of four
`<use>` each in `<defs>` and nothing at the root: the guard counts 0, QtSvg spins (minutes; sampled in
`QSvgHandler::parse`). The same bomb drawn from the root is refused in 0.3 s. The fuzzer's deck had the
root's `<use>` cut off by a parse error, which makes it this.

On the GUI thread for a picture (`ImageDoc`), a deck's picture (`Media::probe`), a Markdown image and
an embedded image (the guard's four users).

**Fixed**: after the root's count, every scope not yet counted that holds a `<use>` is counted too, and
the most any of them asks for is the answer (the counts are kept, so each scope is walked once). Tests in
`test_pptx_doc`'s `svgAskingTooMuchIsNotDrawn`: the bomb with nothing at the root drawing it, and one cut
off by bad XML before the root's `<use>`, refused.

## H2. Closing a 3D model whose build does not end

`msm_photodiode.qfem3d` with an Array's count `["N", "2147483647"]`: its geometry builds in the
background for ever (an Extrude's sketch, the 2D `Builder` making its copies). Closing its tab
then never returns (as for `chip_heater.qfem3d` with a coordinate of `-1e308`, whose build loops as H3's does): `~Multiphysics3DDoc` waits for the job's thread, which has no way to be told to stop
(sampled: `closeFile` → `~Multiphysics3DDoc` on the main thread, `buildGeometry3` → `buildObjects` →
`Builder::feature` on the job's).

The Array had a budget - 1 to 10,000 copies - but checked as `nx * ny` in ints: 2 by 2147483647 is -2,
under it.

**Fixed**: the counts rounded and compared as doubles, so the Array refuses them ("an array of 1 to 10000
copies"); `~Multiphysics3DDoc` tells its job to stop and waits 2 s for it, then leaves the thread to end
on its own (`finished` deletes it; the job, which the thread's function holds by a `shared_ptr`, has its
own copies of the model and parameters). A test: `test_multiphysics3d_doc`'s `closedWhileItBuilds` - the
2 by 2147483647 Array refused, said; a tab building 400 coincident circles (about 10 s, no question asked
whether to stop) closed in 2 s, its thread gone some seconds later. With H3's fix, `chip_heater`'s
coordinate of -1e308 is refused before its plan is triangulated.

## H3. A rectangle 1e308 tall

`soi_waveguide_modes.qfem` with the PML rectangle's height `H + 1e308*d`: opening it never returns -
`MultiphysicsDoc::load` → `buildGeometry` → `formUnionOf` → `CDT::Triangulation::insertVertices`, its edge
flips without end (sampled), on the GUI thread. The hunt of 21 October gave far points a guard (G1, G2);
this one is a size.

**Fixed**: `formUnionOf()` refuses a geometry past 1e60 of its unit (or not finite) before it is
triangulated: the in-circle test multiplies four of its differences, past a double's range at about
1e77. The message is the objects lost to the tolerance where there are any (as at 1e300 since the hunt of
21 October), else "the geometry reaches ..., past what is meshed (1e60)". The 3D plan goes through the
same function. A test: `test_fem_engine`'s `extentsPastMeshingRefused` - the SOI waveguide with `H +
1e308*d`, 1e70 and 1e300, and a square 1e70 alone, refused; one 1e20 built.

## H4. A Markdown run that is never done breaking

`test_markdown_doc`'s fuzzer, seed 8, ran past its 40 minutes. Two samples of the test four minutes apart
were the same: `TestMarkdownDoc::fuzz` → `markdown::render` → `markdown::breakLongRuns` →
`QTextFragment::length`. The fuzzer keeps a slow document only once it is done, so its current one was
written out by a line added for the run (and taken out again) and reduced by opening it in the program,
244 tries, to [h4_markdown.md](2026-11-09-crash-hardening/h4_markdown.md):

```
><summary></summary>
>
><b>b</b>}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}}
> 
```

Opened with `open_document` the program never answers. `breakLongRuns()` gives a run past 64 letters a
zero-width space every so many; it walks the document's blocks, and puts each fragment's text back with
its breaks in it, from the block's end. It kept the block's `QTextFragment`s for that, and a
`QTextFragment` is the document's fragment by its number, with the number of the one after it: putting
the run (`}}}...`) back merged the fragment map, and the next one's - `b`, bold - `length()` walked the
fragments for ever looking for an end it no longer had. (The 990-byte document the fuzzer met held a
literal U+FFFC as well, which the reduction took out.)

`claudecodepanel.cpp` calls the same function on every part of the transcript (`kTranscriptRuns`): a
reply of Claude's with these lines would be expected to freeze the panel the same way - not verified.

**Fixed**: each fragment's position, length, format and text are taken before any is put back, and the
pieces worked from those (from the end, so the earlier positions hold). The fuzzer writes its current
document to `QUCS_FUZZ_OUT/markdown-current.md` before rendering it. A test: `test_markdown_doc`'s
`aRunAfterHtmlInAQuoteEnds` - the four lines rendered, and the same two quotes deep with runs of 65, 200
and 4,000. The panel calls the same function: not tested apart.

## Q1. qucsator_rf: a missing Touchstone file

[q1_spfile.net](2026-11-09-crash-hardening/q1_spfile.net): `spfile@sp.net` of qucsator's tests with the
`SPfile` part's nodes damaged to one (`X1t2 gnd`) and its file missing - SIGSEGV at 0x48:

```
0 qucsator_rf  spfile::createIndex()
1 qucsator_rf  spembed::initSP()
2 qucsator_rf  qucs::spsolver::solve()
```

`spembed::initSP()` sets `nPorts` only when the file was read (`data != NULL`) and goes on to compare it
with the part's ports: when the file is missing `nPorts` is whatever it was - on ASan 0xBEBEBEBE, the
allocator's fill, so it says "specifies a -1094795586-port"; in Release 0, which equals a one-node part's
0 ports, and `createIndex()` reads `data->getDependencies()` of NULL.

**Fixed** (upstream code): `nPorts` is 0 in `spfile`'s constructor, and `spembed::initSP()` and
`spdeembed::initSP()` end when the file was not read, saying "file `...` of `X1` could not be read" (the
part then gives nothing, as one whose file lacks a frequency does). A test, the first to run qucsator_rf
in the suite: `test_qucsator_robustness`, the netlist with the part on one, two and three nodes - never a
signal, the file named.

## U1, U2 - undefined behaviour

- **U1**: `<Text 320 350 12 #000000 -2147483648 "...">` in a schematic: `GraphicText::getTransform()`'s
  `rotate(-angle)` negates `INT_MIN` (UBSan at `Schematic::load` → `showAll` → `boundingRect`). **Fixed**:
  the angle read modulo 360 at loading. A test: `test_loader`'s `aTextAtAnyAngle` (-2147483648 is -128,
  2147483647 is 127, both drawn).
- **U2**: `qucs-spowercombining --json` with `{"f": 1e300, "ratio_db": 8.7e11, ...}` (Wilkinson):
  `num2str()` takes `int(log10(|x|)/3)` of infinity. **Fixed**: a number not finite written as it is. A
  test: the spec through `synthesize_power_combiner` in `test_wishlist_tools`'s `aCombinerIsSynthesized`
  (on UBSan the old program stops and gives no answer).

## Checked and found right

- The 161 tools under 3,823 hostile calls on ASan and Release: no crash, hang or report. A message nested
  past Qt's 1,024 levels is answered with `-32700` and an `id` of null, as JSON-RPC wants (the fuzzer had
  to learn to read it).
- Every viewer under 5,275 monkey steps on ASan, builds and solves closed under their tabs among them.
- 22,854 damaged files on ASan of every kind but the multiphysics models (schematics, data displays,
  symbols, RF 2.5D models with their layouts, GDSII, OASIS, `.lyp`, PDF, decks, Markdown, CSV, TSV,
  `.xlsx`, ZIP, SVG, HFSS and EMX stacks, Touchstone, Lumerical `.ldf`): only H1 and U1.
- The bundled programs' `--json`: only U2. The 26 "hangs" the fuzzer reported were its 2-4 MB specs on a
  machine this busy - each answers at once when run again.
- `test_gui_monkey` seed 7 past step 429 when the label's dialog does not open: 3,000 steps clean.
- The damaged-`UNITS` GDS of C1 in the layout viewer: read, listed and drawn without harm.

## Reproducing

Builds as CI makes them (`build-rel`, and `build-asan` with `-fsanitize=address,undefined`). Each fuzzer
takes the program and a scratch folder; the seeds are the examples, `qucs/tests/data`, decks from
`2026-11-08-pptx-viewer/mkdecks.py` and a few made by the program itself (an RF 2.5D model, its stacks,
a data display):

```
QUCS=build-rel/qucs/qucs-s.app/Contents/MacOS/qucs-s python3 fuzz_docs.py SEEDS OUT 1 1200 [qfem,qfem3d]
QUCS=... python3 fuzz_tools.py SEEDS OUT 1 1500
QUCS=... python3 fuzz_viewers.py SEEDS OUT 1 1500
python3 fuzz_tooljson.py build-asan TOOLS.json OUT 1 1500 3
python3 fuzz_cli.py --corpus NETS --ext .net --out OUT --seconds 1200 --jobs 3 --mutate-aux -- build-rel/qucsator_rf/src/qucsator_rf -i '{in}' -o '{out}'
QUCS=build-asan/... python3 repro_c2.py SCRATCH
QUCS=build-rel/... EXAMPLES=qucs-s-26.1.1/examples python3 repro_all.py SCRATCH
```

`repro_all.py` opens each other finding's reproducer in a server of its own (C1's die and GDS, C3's mesh,
H1's SVG, H2's two models closed, H3's waveguide, H4's Markdown): with the fixes every one is answered in
under a second, C3's mesh refused in 10 s ("about 2.3e+11 prisms (765365 plan triangles, 300001
heights)").

Each server runs offscreen with a HOME, settings and workspace of its own, and `QUCS_CLAUDE` pointing at
nothing; `trash_file`, `clean_scratch`, `git_clone`, `git_remote`, `console`, `trigger_action` and the
dialog tools are not fuzzed (the system's trash, the network, a shell).
