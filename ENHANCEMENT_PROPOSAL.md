# Qucs-S Enhancement Proposal

Baseline: upstream ra3xdh/qucs_s **26.1.1** as vendored in `qucs-s-26.1.1/`.
Companion document: [ARCHITECTURE.md](ARCHITECTURE.md).

This proposal is grounded in three sources: the code as read for the
architecture overview, the upstream issue tracker (~55 crash reports since
2017, one still open against 26.1.1), and the upstream release notes. Every
claim about the code below cites a file and, where verified, a line number.

The short version: **the crashes you saw are not random.** They fall into a
handful of well-defined classes, each traceable to a structural weakness in
the codebase. Fixing those classes — not individual symptoms — is the first
workstream. Everything else builds on that foundation.

---

## 1. Why it crashes — evidence and root causes

### 1.1 What upstream users report

Searching the upstream tracker for crash/segfault reports and grouping them
by trigger:

| Trigger class | Representative issues | Status in 26.1.1 |
|---|---|---|
| **Opening a data display / plotting results** | #1711 (open, stack trace attached, filed against 26.1.1), #1539 "too big .dat.ngspice crashes", #1127 "corrupted data file crashes", #1526 "parametric noise analysis crashes", #5 "voluminous Xyce output can crash" | #1711 **unfixed**; others patched case-by-case |
| **Editing wires, labels, nodes** | #1678 (closed *after* 26.1.1 as "cannot reproduce"), #1443, #1398, #1388, #1377, #1308, #1302, #1298, #1273, #1245 | Cluster appeared after the 25.2.0 "massive redesign of schematic rendering engine"; fixed individually |
| **Simulation lifecycle** | #1543 (Qt 6.4.2 `forkfd_pidfd` bug), #979 "crash if no simulator found on first start", #562 "closes when closing the tuner" | Qt bug is environmental; tuner lifetime is a code issue (see 1.3) |
| **Settings / dialogs** | #1340 "Application Settings segfault", #902 "renaming missing sub-circuits" | Fixed individually |

Two observations. First, the same *classes* keep recurring even though each
*instance* gets fixed — which means the fixes are patching symptoms. Second,
#1711 is the most likely match for what you experienced with the 26.1.1
binary: it is a segfault when opening a schematic from an example project,
reproducible with the attached zip, and its stack trace points straight at
the code analysed next.

### 1.2 Root cause A — the dataset loader trusts the file

Stack trace from #1711 (26.1.1, Fedora 44):

```
#0  Diagram::calcData(Graph*)
#1  Diagram::updateGraphData()
#2  Schematic::reloadGraphs()
#3  QucsApp::slotChangeView()
#4  QucsApp::gotoPage(QString const&, bool)
```

What the code does (`qucs-s-26.1.1/qucs/diagrams/diagram.cpp`):

- `Graph::loadDatFile` (line 822) reads the whole `.dat` file into a
  `char*` and scans it with hand-rolled `strtod` loops. For a dependent
  variable it computes `counting = ∏(independent-variable counts)` from the
  `<dep name indep1 indep2>` header, allocates `new double[2*counting]`, and
  then reads **exactly `counting` numbers with no end-of-buffer check**
  (lines ~1000–1030: `pPos = pEnd + 1` after every `strtod`, even when
  `*pEnd == '\0'`). A dataset that is truncated (aborted simulation),
  malformed (simulator wrote garbage — #1127), or simply stale relative to
  the `.dpl` that references it reads past the buffer.
- The "digital" branch (lines ~1045–1075) calls `realloc()` on a buffer that
  was allocated with `new[]` and is later freed with `delete[]` — undefined
  behaviour on every platform.
- `Diagram::calcData` (line 521) then walks `cPointsY` with raw pointer
  arithmetic (`pz += 2` per point, `countY` branches) and does `(p - 3)`
  look-behind on the output buffer, which underflows when a branch has fewer
  than three points.

None of this code is exercised by any test. The `.dat` format is trivially
fuzzable and the examples directory already contains a corpus.

### 1.3 Root cause B — object lifetime is unmanaged

The document model is five `std::list<T*>` of raw owning pointers
(`Schematic::a_DocComps` etc.). Three consequences, all verified:

1. **Undo leaks and orphans everything.** `Schematic::rebuild()`
   (`qucs/schematic_file.cpp:1314`) does `a_DocWires.clear(); a_DocNodes.clear();
   …` — `std::list::clear()` on raw pointers — then re-parses the snapshot
   string into brand-new objects. Every undo/redo leaks the previous document
   and silently invalidates every pointer anyone else is holding.
2. **The tuner holds raw `Component*`** (`qucs/dialogs/tuner.h:66`). It
   subscribes to `signalComponentDeleted`, but `rebuild()` never emits it, so
   after an undo the tuner writes into orphaned components. This is the
   mechanism behind #562-style crashes.
3. **`MouseActions` caches `focusElement`, `selElem`, `movingElements`**
   (`qucs/mouseactions.h`). Only `selElem` is reset on document switch
   (`qucs/schematic.cpp:2087`); nothing resets them on undo, delete-selection,
   or reload. The 2025 cluster of wire/label editing crashes is consistent
   with a stale element pointer being dereferenced one event later.

Additionally, the simulation dialog is never freed: `slotSimulateWithSpice`
does `new ExternSimDialog(...)` (`qucs/qucs.cpp:3710`) and the matching
`delete` is commented out (line 3728); the dialog has no
`WA_DeleteOnClose`. Each holds a raw `Schematic*`, so closing a document tab
while its non-modal DC-bias simulation is running leaves a dangling pointer
that fires on completion.

### 1.4 Root cause C — unchecked downcasts

`QucsApp` holds documents in a `QTabWidget` that can contain either a
`Schematic` or a `TextDoc`. There are **27** C-style casts of
`DocumentTab->currentWidget()` to `Schematic*` in `qucs/*.cpp`; only 3 have an
`isTextDocument()` guard nearby. One concrete case:
`QucsApp::slotAfterSpiceSimulation` (`qucs/qucs.cpp:3827`) casts *whatever
tab is current when the simulator finishes*, not the schematic that was
simulated. Start a DC-bias run (non-modal), switch to a `.va` or `.m` tab,
wait — crash.

### 1.5 Root cause D — invariants are checked only in Debug

There are 110 `assert()` calls in `qucs/`. The Release build (which is what
CMake defaults to and what every binary is) compiles them out via `NDEBUG`
and additionally passes `-w` (`qucs/CMakeLists.txt:111`), suppressing all
compiler warnings. So the invariant violations that surface as clean
"Assertion failed" reports in debug builds (#1443, #1308, #1377) continue
silently in the shipped binaries and corrupt state until something else
crashes — typically far from the cause, which is why upstream often cannot
reproduce.

### 1.5b What the crash reports on this Mac say

`~/Library/Logs/DiagnosticReports` held nine `qucs-s` reports from one
session with the upstream 26.1.99 continuous build (2026-09-17). They are
two bugs, and neither is in the dataset loader:

| Reports | Crashed in | Cause | Status |
|---|---|---|---|
| 8 | `QLabel::setText` ← `QucsApp::setDocumentTabChanged` ← `TextDoc::slotSetChanged` ← `TextDoc::~TextDoc` | **macOS only.** Destroying a text tab's syntax highlighter emits a "modified" notification (Qt 6.10); the `#ifdef __APPLE__` branch of `setDocumentTabChanged` C-casts `tabButton(currentIndex())` to `QLabel*` and calls it — null once the tab is gone. Linux/Windows use `setTabIcon`, which tolerates it, so upstream cannot reproduce. `slotFileChanged` also marked whatever tab was *current*, not the emitter's. | Fixed: bounds/null checks, `sender()`-based tab lookup, highlighter detached before the document emits. `test_crash_reports::modifiedSignalFromADocumentWithoutATabIsIgnored` reproduces the exact signature on the old code. |
| 1 | `Schematic::adjustPortNumbers` +8864 ← `save` ← Save All | **All platforms, still in upstream `current`.** The Verilog-A branch compares `((PortSymbol*)pp)->numberStr` with `pp` still `nullptr` (the loop variable is `painting`). Saving any Verilog-A module schematic with port symbols crashes deterministically. Confirmed by disassembling the installed binary at the reported offset (loop-invariant pointer = `0x50` = `&nullptr->numberStr.size`). | Fixed (one identifier); worth an upstream PR. `test_crash_reports::savingAVerilogASymbolSchematicDoesNotCrash` reproduces it on the old code. |

The session was evidently Verilog-A work (editing `.va` text tabs, then
saving the module schematic), which the WS3 items around headless
robustness and the text editor should keep in mind.

### 1.6 Root cause E — platform / toolchain

- The Intel-Mac release binary is built against **Qt 6.2.4** (2022) while the
  Apple-Silicon universal build uses Qt 6.10.3
  (`.github/workflows/deploy.yml:151`). Two very different Qt runtimes are
  shipping under one version number.
- Distro builds against Qt 6.4.x crash on every `QProcess::start()` because
  of Qt's `forkfd_pidfd` bug (#1543). Not our bug, but the app has no
  fallback or diagnostic — it just dies when you press Simulate.

---

## 2. Proposed workstreams

Ordered by leverage: each workstream makes the next one safer to do.

### WS1 — Crash hardening (highest priority)

Target the *classes* from §1, not the individual issues.

**Progress:**

- 1.1 / 1.2 done (`qucs/diagrams/diagram.cpp`, `graph.cpp`, `tabdiagram.cpp`).
  The loader refuses sample counts that cannot fit in the file, never walks
  past the buffer, uses `nothrow` `new[]`, leaves a graph completely empty on
  any failure (`Graph::clearData()`), and `calcData` bounds-checks its
  look-behinds. Guarded by the blocking `hostile` smoke suite in CI (21
  cases). Verified against real ngspice AC/transient output.
- 1.3 done. `Schematic` now owns its elements for real: `deleteAllElements()`
  / `deleteSymbolPaintings()` run on undo, redo, reload and in the destructor
  (previously every undo leaked the whole document and closing a tab leaked
  it too), and `signalDocumentRebuilt(Schematic*)` tells holders of element
  pointers to let go. `MouseActions::forgetDocumentElements()` drops the
  element under the mouse, the active diagram and the dragged selection and
  aborts an in-flight drag; the three drag handlers that dereferenced
  `focusElement` blindly now bail out cleanly. The tuner remembers component
  and property *names* and re-binds after a rebuild (or drops the row), and
  detaches when the tuned document is closed. Also found on the way: an undo
  depth of 0 (allowed by the settings dialog) made every document open read
  freed memory — clamped now — and a "wrong document version" prompt blocked
  headless runs forever. Guarded by `qucs/tests/test_schematic_lifetime`
  (QtTest, runs under ASan in CI).
- 1.5 done. `QucsApp::currentSchematic()` (a `qobject_cast`) replaces all 27
  C-style casts of the current tab; every call site handles the text-document
  case. `slotAfterSpiceSimulation` now acts on the schematic the dialog
  simulated instead of whatever tab is current. The 24 remaining indirect
  casts are all inside `isTextDocument()` branches.
- 1.4 done. `ExternSimDialog` is `WA_DeleteOnClose` (one was leaked per
  simulation - per slider step when tuning - for the life of the document;
  the dangling-pointer half was already covered by the dialog being a child
  of the schematic). `SimMessage` keeps its document in a `QPointer` and the
  post-simulation code checks it, so closing a document during a Qucsator
  or ASCO run no longer dereferences a freed widget.
- 1.6 done. `QUCS_ASSERT` (`qucs/qucs_assert.h`) replaces all 110 `assert()`
  calls in the application: identical in Debug, and in Release it logs the
  violated invariant (file:line) instead of vanishing - the log is also part
  of the crash report. Release builds compile with `-Wall -Wextra` again (the
  two clang-only cosmetic categories are silenced); the warnings that were
  hidden turned up an uninitialised `hasY1` in the diagram readout, three
  unchecked file opens, `PTRDIFF_MIN` used as a float bound, and a few
  deprecated captures - all fixed. 8 Qt-deprecation notices remain.
- 1.7 done. `qucs/crashhandler.*` catches fatal signals and uncaught
  exceptions, writes `crash-<time>.txt` (version, commit, Qt, OS, signal,
  backtrace, last 64 log lines) under the app-data directory, autosaves
  modified documents, then lets the OS crash report happen; a session marker
  detects an unclean exit. `qucs/autosave.*` keeps an atomic copy of every
  modified document (2-minute timer, `AutosaveInterval` setting; 0 disables)
  with a `.meta` sidecar, removed on save/close/clean exit. On the next start
  the user is told about the crash and offered the documents, which reopen
  under their original names marked modified. `QucsDoc::writeTo()` serialises
  a document anywhere without touching its state. Covered by
  `qucs/tests/test_autosave` including two real crashes in child processes.
  Verifying the macOS bundle found a quit bug that predates all of this:
  `QucsApp::closeEvent` called `qApp->quit()` from inside the close event.
  Qt on macOS implements `quit()` as `[NSApp terminate:]`, and Cmd-Q, the
  application menu and `slotFileQuit` all deliver that close event from
  within AppKit's termination sequence, so the nested `terminate:` made
  AppKit call `exit()` directly - `QApplication::exec()` never returned,
  `aboutToQuit` never fired, and nothing after `exec()` in `main()` ran
  (confirmed with a breakpoint on `exit`). The quit is now deferred to the
  next event-loop pass; every quit path (Cmd-Q, application menu, window
  close button) exits through `main()` with status 0.
- 1.8 (new, from fuzzing): `scripts/ci/fuzz-sch.py` mutates the example
  schematics and runs every mutant through the netlister and the renderer
  under ASan/UBSan; 150 seeded mutants run on every CI push. The first
  3,500 mutants found seven classes of damage the loader could not take:
  - a two-part version header (`<Qucs Schematic 26.1>`) indexed past the
    end of the split list in `VersionTriplet`;
  - blank lines or a lone `<` inside any section were indexed at `[0]`/`[1]`
    before the length was checked (six loaders, `Component::load`,
    `Wire::load`, `Diagram::load`, `Marker::load`, `Graph::load`);
  - an emptied property value (`""`) was read with `at(0)` in twelve
    component classes and in `Diagram::load`'s optional fields;
  - a missing quote shifted the property/display fields so the display flag
    was read past its end;
  - a marker line before any graph called `QList::last()` on an empty list;
  - coordinates near `INT_MAX` overflowed the bounding-box, margin and
    distance arithmetic (Qt 6.11 asserts on it in Debug; `distance()`
    overflowed for any wire longer than 46,340 units); every coordinate
    read from a file is now clamped to `misc::MaxCoordinate` and distances
    are computed in double;
  - the rotation field was used as a loop bound (`rotate()` 2^31 times), and
    a zero grid size reached `setOnGrid()`'s integer division (also from an
    emptied field in the document-settings dialog).
  In Debug builds each of these aborted; in Release they read out of bounds,
  wrapped, or hung. Separately, thirty-four `QMessageBox::critical` calls in
  the load/save paths went through `misc::reportError`, which logs instead
  when there is no GUI: a truncated file used to hang `qucs-s -n`/`-p`
  forever on a dialog nobody could click. `qucs/tests/test_loader` covers
  every class; the pre-fix loader aborts on it.
  Wiring datasets into the fuzzer exposed a gap in the `simulate` smoke
  suite: it wrote the converted ngspice output as `<name>.dat`, while a
  diagram trace named `ngspice/...` is loaded from `<name>.dat.ngspice`
  (what the GUI writes), so its render step had never loaded any data. It
  now writes the file the renderer looks for and fails if the render log
  does not show the dataset being loaded; the fuzzer's `--extra` mode then
  mutates those real datasets as well as the schematics.
- 1.9 (new): the simulator-output parsers (`AbstractSpiceKernel::parse*`,
  `convertToQucsData`) - the code behind every "Simulate" click and the
  home of upstream #1526 / #5 - got the same treatment.
  `qucs/tests/simout_harness` converts a directory of ngspice output files
  exactly as the GUI does (its baseline output is byte-identical to the
  CLI's datasets for all 14 corpus circuits), `scripts/ci/fuzz-simout.py`
  damages one file per mutant, and the `simulate` suite now covers every
  format the parsers know (parameter sweeps with `.cir.res`, noise,
  sensitivity, distortion, custom nutmeg scripts, S-parameters) and stashes
  the raw files as the corpus. First finding: a raw file whose header says
  `No. Variables: 2147483647` looped two billion times on an empty stream,
  and an operating-point plot without a following plot spun forever at EOF;
  a truncated binary section produced short rows that the conversion then
  indexed past the end. The parsers are now bounded by what the file really
  contains, keep only complete rows, and the conversion drops any short row
  before indexing. `qucs/tests/test_simout` covers it (three of its cases
  hang on the pre-fix code). 3,300 mutants clean so far.
- 1.10 (new): stress. The fuzzers only reach what a file can say; the
  interactive half of the application had no such test. `qucs/tests/
  test_gui_monkey` walks the main window at random over copies of the
  examples (gestures in every mouse mode, drags interrupted by undo or
  delete, the menu commands, tabs, symbol view, hierarchy, component
  placement, every dialog cancelled or filled with odd values and
  accepted) under ASan/UBSan, with a watchdog for hangs. On the first 96
  walks of 1,500 steps, 94 died. What it found:
  - the element under the mouse outlived its element: a double click on a
    wire is press, release, double click, and the release's healing merged
    the wire away before the double click edited it; a delete in the
    middle of a drag, or of a Move-mode drag, freed what the drag held.
    `Schematic::holds()` / `heldElements()` say what the view shows now,
    and `MouseActions::dropStaleElements()` runs before every handler and
    every command that reads `focusElement` (five in `QucsApp`);
  - one component in two documents: the lists symbol mode uses for
    components, wires, nodes and diagrams were globals, and a double click
    in the symbol view edited the component picked in the schematic view,
    whose dialog then put the copy into those shared lists - so every open
    symbol listed it and closing one document freed it for all. They are
    per document now (and freed with it);
  - the healer freed a wire twice (both its ends misplaced plan its
    deletion twice) and left a wire with a lost end in the list, which the
    next step dereferenced and a later action of the same heal split
    towards the end it no longer had; a node two ports of one component share (port
    symbols dragged onto each other) was freed twice on Save All;
  - a zoom rectangle released outside the canvas (Debug abort), a paste
    with nothing on the clipboard, a labelled wire of no length (0/0),
    "1e308k" in a component dialog (log10(inf) to int), the search dialog
    closed after its document, a subcircuit instance without properties;
  - a hang: limits set by a zoom rectangle on a collapsed axis became nan,
    the log axis took nan for valid and its grid loop never moved on. All
    six grid loops are bounded (`Diagram::MaxGridLines`), nan limits are
    invalid, and every grid position goes through `Diagram::gridPixel()`.
  After the fixes, 412 further walks of 2,000 steps turned up two more
  (the shared node and the split wire above); what still stops a walk
  now and then is a Debug-only healer invariant (below). 3,000 more
  schematic mutants and 1,500 mutants of a real NgSweep run's output
  (`ngsweep::datasetBlocks`, new code) came through clean.
  A scale suite (chains of 16,000 components, 160x160 wire meshes,
  5-million-point waveforms in every diagram type, 20,000-curve sweep
  families, 400 diagrams, 400-deep hierarchies, 8,000 labels on one net)
  found three more: a subcircuit that includes itself (directly or through
  others) sent the four walks that collect SPICE libraries, library files,
  Verilog-A files and .spiceinit blocks down the hierarchy until the stack
  ran out (the netlister itself always knew each file once) - they keep a
  visited set now; a property value of a megabyte (a PWL list of many
  thousand points) made the unit patterns of `spicecompat::normalize_value`
  backtrack over every split of its digits, and the netlist never came
  (possessive quantifiers: 0.3 s for 10 MB); and the same value, shown on
  the canvas, stretched the drawing so that an image export asked for 43 GB.
  The canvas shows 1,000 characters of a value (`Property::displayText()`)
  and a raster export is fitted into 100 megapixels, 32,000 a side.
  `qucs/tests/test_stress_findings` has a test for each finding (the ones
  that compile against the old code fail or hang there). Written up in
  [docs/bug_hunts/2026-09-24-stress.md](docs/bug_hunts/2026-09-24-stress.md).
  Loading and every healing edit were quadratic in the size of the
  schematic (a node search per pin, all-pairs checks over the whole
  document, elements leaving the lists one search at a time): at 16,000
  components a load took 3 s and a rotate of one resistor 9 s. Fixed in
  `46332a5`:
  the loader and the healer's node replacements look nodes up in a hash by
  place and wires in a grid (`conductor_index.h`), healing and its
  invariant checks find the nodes on a wire from the nodes sorted by row and
  column, deletions go in one pass, and a paste numbers its components from
  a table - 0.46 s, 0.10 s and 0.27 s for load, rotate and undo at 16,000,
  and four times the size costs four times the time
  (`qucs/tests/test_large_schematics` holds it to that). On the way, a
  monkey walk found a subcircuit pin numbered 2147483647 making 16.7
  million pins and hanging the load (numbers above 100,000 are refused),
  and that healing depended on node addresses, so a partial edit could
  end differently from run to run (it goes by place now, `62b04d4`; the
  walks that followed found a pasted component keeping the closed document
  it was parsed in, and Close All freeing the canvas property editor -
  both fixed there too).
  Still open: two healer invariants
  fail - a node left within a unit or two of a diagonal wire without being
  connected, and labelled nodes with nothing under them when a component's
  library file is not found (Debug aborts; Release logs).
- Infrastructure that fell out of 1.3: the core sources are an object
  library (`qucs-core`) shared by the executable and `qucs/tests/`; the
  globals moved from `main.cpp` to `globals.cpp`; and the top-level CMake no
  longer forces Release, so Debug builds (asserts, `-Wall -Wextra`, debug
  output) work for the first time — CI's sanitizer job is now genuinely
  Debug.

| # | Task | Where | Notes |
|---|---|---|---|
| 1.1 | Rewrite the `.dat` reader as a bounds-checked parser with a defined error result. Reject/skip a variable whose sample count ≠ header, never read past the buffer, replace the `new[]`/`realloc` mix. | `qucs/diagrams/diagram.cpp` `Graph::loadDatFile`, `loadIndepVarData` | Unblocks #1711 and the whole "plotting crashes" class. Keep the parse in a standalone class so it can be unit-tested without Qt widgets. |
| 1.2 | Make `Diagram::calcData` tolerant of short/empty branches; guard the `(p-3)`/`(p-2)` look-behinds. | `qucs/diagrams/diagram.cpp:521` | |
| 1.3 | Emit a `documentRebuilt()` signal from `Schematic::rebuild()` / `rebuildSymbol()` and have `MouseActions`, `TunerDialog`, marker code, and `ExternSimDialog` drop cached pointers on it. Free the old elements before `clear()`. | `qucs/schematic_file.cpp:1314`, `qucs/mouseactions.*`, `qucs/dialogs/tuner.*` | Fixes the undo leak and the dangling-pointer class in one change. |
| 1.4 | Give `ExternSimDialog` `WA_DeleteOnClose` and have it hold the schematic via `QPointer<Schematic>`; make `slotAfterSpiceSimulation` use the dialog's schematic, not `currentWidget()`. | `qucs/qucs.cpp:3710`, `:3827`, `qucs/extsimkernels/externsimdialog.*` | |
| 1.5 | Replace the 27 C-style `(Schematic*)currentWidget()` casts with a single `Schematic* QucsApp::currentSchematic()` helper that returns `nullptr` for text docs; audit each call site for the null case. | `qucs/qucs.cpp`, `qucs_actions.cpp`, `qucs_init.cpp` | Mechanical, low risk, closes an entire class. |
| 1.6 | Keep invariant checks alive in Release: replace `assert()` with a `QUCS_CHECK(cond)` macro that logs + returns gracefully in Release and aborts in Debug. Remove `-w`; fix or explicitly silence the warnings it hides. | `qucs/CMakeLists.txt:111`, all `assert(` sites | Converts silent corruption into logged, recoverable failures. |
| 1.7 | Add a crash-time safety net: install a signal handler / `std::set_terminate` that writes an autosave of every dirty document to the temp dir plus a backtrace, and offer recovery on next launch. | `qucs/main.cpp` | Doesn't fix crashes, but stops them costing work — and gives you stack traces from users. |
| 1.8 | Fuzz the schematic loader: mutate the shipped examples, push every mutant through `-n` and `-p` under the sanitizers, fix what falls over, keep a seeded run in CI. | `scripts/ci/fuzz-sch.py`, the `load()` functions of every element class | The parsers are hand-written `section()`/`at()` code with no length checks; fuzzing finds these in minutes. |
| 1.9 | Fuzz the simulator-output parsers: harness `convertToQucsData` over a directory of real ngspice output, mutate the files, keep a seeded run in CI. | `qucs/tests/simout_harness.cpp`, `scripts/ci/fuzz-simout.py`, `extsimkernels/abstractspicekernel.cpp` | Header counts in raw files were trusted as loop bounds; this is where "voluminous output crashes" (#5) and "noise analysis crashes" (#1526) live. |

### WS2 — Engineering infrastructure

Without this, WS1 regresses.

| # | Task | Rationale |
|---|---|---|
| 2.1 | CI that **builds** on Linux, macOS (arm64 + x86_64) and Windows on every push. Upstream's `deploy.yml` only runs on release. | Catch platform breaks before users do. |
| 2.2 | A Debug + **ASan/UBSan** CI job that runs the app headless over the `examples/` tree (`qucs-s -n -i file.sch`, `-p`, and a new `--open-dpl` smoke mode). | Every root cause in §1 is the kind ASan reports on the first run. The headless CLI modes already exist in `main.cpp`. |
| 2.3 | Unit tests (Catch2 or QtTest) for the pure parsers: `.dat`, `.sch`/`.dpl`, `.lib`, ngspice raw / Xyce `.prn` output. Seed a fuzz corpus from `examples/` and the datasets attached to #1711, #1539, #1127. | These parsers are where the crashes are and they have zero tests today. |
| 2.4 | Pin one Qt version across all release artifacts (currently 6.2.4 vs 6.10.3 on macOS). Detect the Qt 6.4 `forkfd_pidfd` bug at startup and show a clear message instead of crashing on Simulate. | |
| 2.5 | Enable `-Wall -Wextra` in Release and fix the warning backlog incrementally (clang-tidy `bugprone-*` and `cppcoreguidelines-owning-memory` as a starting profile). | |

- 2.1 done: `ci.yml` builds Release on macOS (`macos-15`, with the unit
  tests under the offscreen platform) and on Windows (`windows-2022`,
  MSYS2 ucrt64, GCC) on every push besides the Linux ASan job. Prompted by
  the first `v26.1.2` release run, where five platform-only breaks
  (runner glibc, files hidden by upstream's `.gitignore`, MinGW's
  `open()`, an Apple clang crash, a stale download) surfaced together.
- 2.2, 2.3 done (the ASan job, `qucs/tests/`, the fuzzers). 2.4: one Qt
  (6.10.3) on every platform.

### WS3 — UI/UX enhancements

This is the stated purpose of the repo. Items are ordered by (user demand ×
feasibility given the current architecture); upstream issue numbers indicate
existing demand.

**Quick wins (days each, no architectural change)**

- *Done (with WS1.7):* **Crash recovery / autosave**: periodic autosave,
  "Recover unsaved documents" on launch.
- *Done:* **Verilog-A "Build All"** on the Content panel's Verilog-A row:
  compiles every `.va` of the project with the OpenVAF from the settings,
  sequentially and asynchronously (the GUI stays responsive), output and a
  pass/fail tally in the message dock, `.osdi` files appear in the tree.
  Points at the settings when OpenVAF is not configured. Covered by
  `qucs/tests/test_build_all_va` with a stand-in compiler.
- *Done:* **Libraries that bring their Verilog-A.** *Create Library*
  wrote the subcircuits' netlists and symbols; the Verilog-A their
  `.model` cards need stayed in the project, so a library with Verilog-A
  in it could not simulate anywhere else. `LibraryDialog::embedVerilogA()`
  (when `QucsSettings.EmbedVerilogAInLibraries`, on by default, *Settings*
  tab) takes the SPICE netlist of each subcircuit, `osdi::usedModelTypes()`
  of it, and from the project's files and those of the libraries of the
  subcircuit's components (`AbstractSpiceKernel::collectVerilogAFiles()`,
  the new `Component::getVerilogAFiles()`) copies into the library's
  folder the sources that define the modules and the files they
  `` `include `` (their relative paths kept; one from outside the
  source's folder is warned about), listed in the component's
  `<SpiceAttach>` - which older versions read and `getSpiceLibrary()`
  ignores for anything but netlists. No compiled model goes in: an
  `.osdi` runs on one platform only; one a library made before had
  compiled beside a source that is copied again is removed, and a module
  the project has only compiled is warned about. Two files that would
  share a name, a failed copy, fail the library. `LibComp::
  getVerilogAFiles()` gives back the sources and the models compiled
  beside them; `Ngspice::osdiLoads()` and `verilogABuilds()` take them
  with the project's - and without a project too: `osdi::builds()`
  compiles a source with no model (`Build::missing`) before the
  simulation, OpenVAF writing it beside the source. A model built for
  another platform (`osdi::builtForAnotherPlatform()`: the format and
  processors of an ELF, Mach-O - universal too - or PE header against the
  ngspice program's own, through PATH and links; without a program to
  read, this platform's format and, off macOS, its processor) is not
  loaded (a netlist comment says so) and is compiled again
  (`Build::foreign`; the status log says why) - a library folder shared
  between machines. A header of no known format is not judged: the test
  stand-ins, and libraries Qucs-S cannot load itself but ngspice can, are
  read for the module's name as before. `test_library_verilog_a` covers
  the headers (formats, processors, universal binaries, a script for a
  simulator), a library made with the setting on (the source and the
  file it includes; no model, the stale one removed; not the unused
  module), a module with no source, the setting off, a circuit outside
  the project that uses the library (compiled first, then the `pre_osdi`
  line; with a foreign model none, the note, and a build again), and the
  setting in the dialog; `test_osdi_selection` checks libraries OpenVAF
  really built against this platform, the test program and the ngspice
  on PATH.
- *Done:* **"Compile" on a .va file** of the Content panel: the file
  menu offers it on a `.va` row (the right-clicked file, or the `.va`
  files selected with it: *Compile N Files*), through the same queue as
  Build All (`QucsApp::buildVerilogA()`: the checks, then
  `startNextVerilogABuild()`). Open files with changes can be saved
  first (*Save and Compile*) or compiled as saved. The output now comes
  to the front (`MessageDock::showBuildOutput()`): the dock is tabbed
  with the simulation console, terminal and Python shell, and only
  `show()` was called, which left it behind the tab in front - and inside
  the dock the Problems or Operating Point tab could be the one shown.
  `test_build_all_va` covers the menu (only on `.va` rows), one file,
  a selection, *Save and Compile*, and the output in front of the
  console (by its visible region: a tabbed dock behind another stays
  "visible", moved out of sight). Found on the way by UBSan: a text
  document saved its `Recreate=` setting from a flag nothing had set,
  and *Delete*, *Activate* and *Zoom In* cast the tab in front to a text
  document before asking whether it was one (`qobject_cast` now).
- *Done:* **Verilog-A components as their module describes them.** A
  module becomes a component through JSON files written when its symbol is
  saved (`NAME_props.json`, `NAME_sym.json`, merged into
  `NAME_symbol.json`) and read by `vacomponent`. Upstream wrote them by
  hand - unescaped strings, trailing commas - and read them by deleting
  every space in the file before parsing (to get rid of the commas): every
  description lost its spaces, a quote in one parameter's description
  made the whole file invalid and the component came up without
  parameters (reported as "Symbol file not found"), a quote in a text on
  the symbol did the same. The merge glued the two files by deleting
  "}{" and looked for them in the project folder rather than next to the
  symbol. The OSDI reader took descriptor 0 without checking the library
  (`osdi_log` written through unchecked, no version or count), read a
  string parameter's pointer as its text and integers as unsigned, and
  the fallback source reader indexed past its token list; the units were
  a TODO. `qucs_s::vamodule` (`qucs/vamodule.*`) now reads the library
  (OSDI 0.3 and later, descriptors stepped by `OSDI_DESCRIPTOR_SIZE`, the
  module of the file's name, `$`-parameters left out, strings quoted as
  `.model` needs them - ngspice refuses `kind=nmos`) while it is not older
  than the source, and the source otherwise (attributes `desc`, `units`,
  `type="instance"`, comments, lists, ranges, local parameters left out);
  writes the files with `QJsonDocument`; `parseJson()` drops only the
  commas before a closing bracket outside strings, so older files still
  read; `jsonString()` quotes a symbol text; `setIcon()` is the load
  dialog's icon change (which rewrote the file as Latin-1, even when
  cancelled). *Load Verilog-A module* finds components anywhere in the
  project. `qucs/tests/test_verilog_a_components` covers the source
  reader, the properties with units, older files, a symbol with texts and
  quotes in a project subfolder, the icon, and - with OpenVAF on the
  machine - the library (defaults, units, instance parameters, strings,
  two modules, the library used only while it is as new as the source).
- *Done:* **Content panel lists the whole project tree.** Upstream only
  showed the files in the project directory itself. `misc::projectFiles()`
  walks the subdirectories (no hidden directories, no symlinked ones) and
  the tree shows `sub/dir/name.ext` under the category, root files first.
  Everything that took the row text as a file name relative to the project
  still does (open, delete, subcircuit insert); copy and rename keep the
  file in its subdirectory, and `misc::properAbsFileName()` resolves such a
  relative path against the project so an inserted `sub/x.sch` netlists
  from any schematic. New *Osdi* category for the compiled models; the
  ngspice netlister looks for `.osdi` files in the whole tree (and loads
  the ones the netlist uses, see below) and Build All compiles every
  `.va` of the tree, so the panel and the simulator agree on what
  belongs to the project.
  Two listings, switched from the context menu of the panel's empty area
  (*Toggle hierarchy search view*; rows keep their own menus) and kept in
  the settings (`ContentTreeView`): the flat
  `dir/name` rows, or a sub-tree per directory under each category. To
  make that possible every file row carries its project-relative path in
  `ProjectView::FilePathRole`, and the consumers in `QucsApp` (open,
  duplicate, rename, delete, insert subcircuit, drag) ask
  `ProjectView::filePath()/isFile()/categoryOf()` instead of reading the
  row text and assuming the parent is the category. Expanded rows
  (categories and folders) are remembered across refreshes by path.
- *Done:* **Only the OSDI libraries a netlist uses.** `Ngspice::
  createNetlist()` wrote `pre_osdi` for every `.osdi` of the project,
  whatever the circuit used: each simulation loaded every library (a
  project that keeps several PDKs' compiled models loaded all of them),
  and two builds of a module (`psp103.osdi` and `old/psp103.osdi`)
  clashed - the fork refuses a second registration. The DC-bias netlist
  (`a_DC_OP_only`) returned before that block and loaded none, so a
  circuit with a Verilog-A device could not show its bias.
  `qucs_s::osdi` (`qucs/osdiselection.*`): `modelTypes()` reads the types
  of the `.model` cards (continuation lines joined, comments left out,
  `type(` and `type (`), `includedFiles()` the files of `.include`,
  `.inc` and `.lib` lines (quoted or not, relative to the file that names
  them), `usedModelTypes()` follows them through the files they include
  (each once, remembered while unchanged); `modulesOf()` asks the
  library itself (`vamodule::osdiModules()`, which shares the loading and
  the checks with `readOsdi()`) and remembers it while the file is
  unchanged, and a library Qucs-S cannot load - built for another
  architecture than Qucs-S, while ngspice may run under Rosetta - is
  searched for the name as a string of its own (a NUL on each side, any
  case); `needed()` keeps one library for each module used: one already
  kept, else the one that has the most of what is still needed, else the
  most recently built, and says what it left out. The netlist body goes
  through a string, the `pre_osdi` lines (with a comment for each module
  left out elsewhere) go into both `.control` blocks - the simulations'
  and the DC bias'. `qucs/tests/test_osdi_selection` covers the cards,
  the includes, the files they include (a loop, a changed file), a
  foreign library, the choice among duplicates and multi-module
  libraries, real libraries compiled by OpenVAF when it is installed,
  and the netlist of a project (a `.MODEL` card, an included library
  including another, a duplicate, unused libraries) for a simulation
  and for the DC bias; `test_build_all_va` checks that a circuit without
  Verilog-A loads none.
- *Done:* **Verilog-A compiled when a simulation needs it.** A changed
  `.va` took a Build All (or *Build Verilog-A module*) before ngspice saw
  it; forgotten, the simulation ran the old library without a word, and
  a module never built was an "unknown model type" from ngspice.
  `osdi::builds()` takes the sources of the project that define a module
  the netlist uses (`vamodule::sourceModules()`, comments left out) and
  keeps those whose library - `NAME.osdi` beside `NAME.va`, where OpenVAF
  writes it - is older than the source or than a file it includes
  (`osdi::sourceIncludes()`: `` `include`` lines followed through the
  project; the ones OpenVAF brings, like `disciplines.vams`, are not
  there and do not count), or is not there while no other library of the
  project defines the module (a module built elsewhere is left to that
  library). `Ngspice::verilogABuilds()` writes the netlist to a string for
  the types it uses. `SimulationRun::start()` compiles those first, one
  after the other, asynchronously, the output in the console (kept when
  the simulation starts), a status line for each; then starts again and
  simulates. A source that fails stops the run: the error in the log and
  the console, `simulated()` with the error, no simulator started. Stop
  kills the compiler. Without OpenVAF (the path in the settings) the run
  goes on with the libraries there are and the log names each one out of
  date. A modified `.va` open in the editor is compiled as saved (said).
  ERC: a Verilog-A component (`vacomponent`) whose module is in no
  library and no source of the open project is a warning.
  `test_osdi_selection` covers the modules of a source, the includes,
  what is compiled (never built, changed since built, an include changed,
  a module in another library, a two-module source, unused sources), a
  simulation of a project with a stand-in OpenVAF and ngspice (compiled
  once, not again while unchanged, again after a change; a broken source
  stops the run; without OpenVAF the run goes on and says so) and the
  ERC warning.
- *Done:* **Folder icons in the Content panel are a setting**
  (`ContentFolderIcons`, default off; *Application Settings → Settings*).
  `ProjectView::folderItem()` sets the icon only when it is on, the
  listing remembers what it was built with and
  `applyRefreshSettings()` rebuilds it when that changed. Found on the
  way: the dialog compared the workspace field (canonical path) with the
  stored path verbatim, so with a workspace behind a symlink every Apply
  closed the project; it compares canonically now.
- *Done:* **Python and Images categories** in the Content panel
  (`ProjectView::Python`, `ProjectView::Images`, between SPICE and
  Others; `ProjectView::imageSuffixes()` is the list, also behind
  `QucsApp::fileType()`). Scratch files are classified first, so an image
  written by a simulation stays under Scratch. `test_project_scratch`
  covers the order, the listing and the suffixes.
- *Done:* **`.OPTIONS` first line dropped** (reported with a screenshot
  of the editor). `SpiceOptions::getExpression()` looped from index 1
  and the loader keeps index 0 for `XyceOptionPackage`, while
  `ComponentDialog::writeEquation()` rebuilt `Props` from the lines, so
  an edited section had an option at index 0. Now: the netlister takes
  the package by name; the dialog shows the package in a `QLineEdit` and
  puts it back first on Apply (DEVICE when empty; a `XyceOptionPackage =`
  line typed anyway is taken as the package); `SpiceOptions::load()`
  (`Component::load` made virtual) repairs a file whose first value
  contains `=`. `qucs/tests/test_spice_options` covers the shipped
  layout, an edited list, both file forms and the dialog round trip.
- *Done:* **Generate Netlist** (`Sim.GenerateNetlist`, *Simulation*
  menu): `SimulationRun::writeNetlist(file)` writes the ngspice / SPICE
  OPUS / Xyce netlist without a dialog; `QucsApp::slotGenerateNetlist()`
  asks for a saved schematic, runs the ERC (non-blocking), writes
  `Scratch/<schematic>/spice4qucs.cir`, refreshes the Content panel and
  opens the file; qucsator is declined like *Save netlist* is. Covered in
  `test_project_scratch`.
- *Done:* **Scratch subfolder per schematic.** `misc::scratchDirFor(doc)`
  is `Scratch/<doc relative to the project, without extension>` while a
  project is open (`untitled` for a nameless one, the base name for a
  schematic outside the project) and `scratchDir()` itself otherwise, so
  headless runs stay flat. `AbstractSpiceKernel` (its `a_workdir`, and
  ngspice's `.spiceinit` in it), `SimulationRun` (the folder, `log.txt`)
  and `QucsApp::currentScratchDir()` (Show Last Netlist / Messages: the
  schematic in front, else the one simulated last) use it.
  `test_project_scratch` runs a fake ngspice twice from a project and
  checks the folder, the files, the two actions and that the second run
  reuses the folder.
- *Done:* **Scratch folder per project.** `misc::scratchDir()` is the
  open project's `Scratch/` (created by `QucsApp::useProjectScratch()` on
  open, and by project creation) and the settings' `S4Q_workdir` otherwise,
  so headless runs and the CI smoke suites are unchanged. The SPICE kernels,
  the ngspice `.spiceinit`, the simulation dialog and the netlist viewer
  ask it; `tempFilesDir` (qucsator, `log.txt`) is pointed at the same
  folder while the project is open. The Content panel lists `Scratch/`
  under its own last category, named relative to the folder, and refreshes
  after a simulation (not per tuner step). The raw simulator output is no
  longer deleted after conversion (upstream did that in release builds);
  it is still removed before the next run of the same netlist.
- *Done:* **File types.** `.cir/.ckt/.sp` are Qucs text documents
  (`QucsApp::textDocumentSuffixes()`, always the built-in editor); plain
  text formats (`QucsApp::textFileSuffixes()`: txt, py, md, json, csv,
  shell and C/C++ sources, SPICE side files, ngspice raw plots, the
  per-simulator datasets, ...) go through `editFile()`, i.e. the editor
  from the settings; a File Types entry wins over the
  defaults for anything but schematics/displays/symbols; the program
  `qucs-editor` (`QucsApp::QucsEditorProgram`, a button in the dialog)
  means the built-in editor; the program is everything after the first
  `/` of the entry, so absolute paths work (upstream's `section('/',1,1)`
  cut them). Drops use the same dispatch.
- *Done:* **Drag and drop from the Content panel into the document area.**
  `ProjectView` is a drag source whose drag carries the selected files as
  `file://` URLs (so anything that takes files from a file manager takes
  them). Drops are accepted by the tab widget (tab bar / empty area), by
  `TextDoc` (which otherwise pasted the path as text) and by `Schematic`
  (which already took file-manager drops). All three hand the files to
  `QucsApp::openDroppedFiles()`, which opens each in its viewer via
  `openDroppedFile()`: `.sch/.dpl/.sym` and the Qucs text document types
  through the Content-panel path (`gotoPage`), other text files (probe: no
  NUL in the first 8 KiB) through the text editor from the settings,
  binaries through the user's file-type handlers. Opening is deferred to
  the event loop because the drop target may be the untitled document that
  opening the first file closes — the upstream schematic handler worked
  around that by temporarily marking the document changed; done
  synchronously it is a use-after-free that `test_drop_open` catches under
  ASan. Verified end to end on macOS with CGEvent drags.
- *Done:* **Documents open from the system** (#973). Upstream opened the
  arguments that do not start with `-` from the `QucsApp` constructor -
  option values included (`--dpi 96` went looking for a file "96" and
  warned it was read-only), and every
  QtTest binary that builds a `QucsApp` opened its own command line
  (run with a test function's name, it blocked on "cannot open"). On
  macOS nothing opened at all from the Finder, the Dock or *Open With*:
  the bundle declared no document types, and a document opened that way
  arrives as a `QFileOpenEvent`, which nobody handled. Now
  `QucsApp::openFromSystem()` is the one way in: paths or `file:` URLs
  (`qucs_s::systemopen::localPath()`), a `*_prj` directory opens as the
  project first, a document already open under another name of the same
  file comes to the front, the untitled placeholder goes, the window is
  raised, and what cannot be opened (missing, not a project, another URL
  scheme) is said in one message. `main()` calls it with
  `QCommandLineParser::positionalArguments()` (and `-i`) once the window
  is up; a `qucs_s::systemopen::Receiver`, installed on the application
  right after it exists, keeps `QFileOpenEvent`s until there is a target
  and hands them over in batches, never under a modal dialog (the first
  run's simulator notice, the crash recovery, "correct dataset names?").
  The bundle gets its own `Info.plist` template
  (`qucs/MacOSXBundleInfo.plist.in`): exported UTIs for the three Qucs
  formats, `Default` for `.sch`/`.sym` (other programs use them), `Owner`
  for `.dpl`, `Alternate` for netlists and Verilog-A. Linux: the desktop
  entry lists the MIME types and takes `%F`; `qucs-s-mime.xml`, installed
  as `share/mime/packages/qucs-s.xml`, defines them by extension and by
  the `<Qucs Schematic` first line. Windows: the Inno Setup script
  registers ProgIDs under `OpenWithProgids` and `SupportedTypes` (never an
  extension's default) with `ChangesAssociations`. Checked on macOS
  through Launch Services (`open -a`): a document at launch and a second
  one handed to the running instance. The same check showed that a trial
  run killed under `QUCS_SETTINGS_DIR` left its session marker in the
  user's own application data - the next real start reported an unclean
  exit - so that variable now also moves the crash reports and autosave
  copies. `qucs/tests/test_system_open` covers the paths and URLs, the
  documents, the project, the message, the receiver (before a target,
  batching, under a modal dialog, other events untouched), a
  `QFileOpenEvent` opening a document, and the three platforms' files
  (the built `Info.plist`, the desktop entry against the MIME package and
  real files' first lines, the installer's registry lines).
- *Done:* **Explicit light/dark theme toggle** (#1725): the theme itself
  is the *Theme* setting below; what was left is the canvas.
  Components draw with hard-coded pens (`Qt::darkBlue` alone ~1900
  times in 193 files, in the constructors), so on a dark paper the
  schematic vanished. `qucs_s::ink` (`qucs/ink.*`) is the indirection:
  `ink::Paper` sets the paper for the time of a paint (nested, white
  when none), and `ink::on()` gives a colour (pen, plain brush) to draw
  with on it - itself on light paper; on dark paper (relative luminance
  under 0.18), when its WCAG contrast to the paper is under 3, the HSL
  lightness turned over and raised until it reaches 3, hue, saturation
  and alpha kept; cached per paper. `Schematic::drawContents()` (the
  canvas only) sets the paper from the viewport's background; the
  colours pass through `ink::on()` where they reach the painter:
  `Component::drawSymbol()` (every primitive's `penHint()` and
  `brushHint()`, so the constructors stay as they are), the component's
  texts, open/short marks, selection, pin names and direction marks,
  simulation blocks, wires, nodes, wire labels, all paintings, the
  grid, the frame and the post-paint previews. A diagram on dark paper
  is a white card under a nested light `Paper` (its header bars, grid
  and legend keep their meaning); the DC bias boxes are light
  themselves, only their leader lines are fitted. Prints and exports
  never set a paper, so they are unchanged. `misc::paperColor()` is the
  canvas paper - `QucsSettings.BGColor`, or `ink::darkPaperColour()` in
  the dark theme when `PaperFollowsTheme` (*Appearance*, default off) -
  and `QucsApp::applyPaper()` gives it to every open schematic and the
  inline text editor, from the settings dialog and on
  `QStyleHints::colorSchemeChanged`. `qucs/tests/test_ink` covers the
  mapping, nesting, every colour of every built-in symbol (each shows
  on dark paper), the canvas (a wire comes out light and still blue, a
  diagram is a white card, the print of the same schematic is dark blue
  as ever) and the setting.
- *Done:* **Auto colors for the curves of a graph.** A graph of a swept
  variable holds a curve per value (Graph::countY branches, each ended by
  a BRANCHEND in the screen points), all drawn in the graph's one color.
  `Graph::autoColor` draws each in a color of its own: `drawLines()` keeps
  each line's and stroke's curve (the long-line joining stops at a
  curve's end) and draws them curve by curve with `curveColor()` and
  `curveStyle()`, the symbol styles switch pens at each branch end.
  `autoPalette()` is eight categorical hues in a fixed order validated for
  protanopia/deuteranopia separation (the order is the safeguard) on the
  white card a diagram is drawn on; past eight the colors come round
  (never generated hues), every curve in the graph's own line style. The
  auto graphs of a diagram continue one sequence (`curveOffset()`).
  Point markers (`Graph::pointMarker`: none, auto, or one of seven shapes)
  tell curves apart where colors repeat: `Diagram::calcData()` keeps each
  curve's data points inside the diagram before clipping moves any
  (`addMarkerPoint()`), and `drawPointMarkers()` draws on every point of
  a sparse curve and some 40 pixels apart along a dense one, each curve
  starting a quarter step on from the one before; filled shapes with a
  white ring (strokes for the cross and plus), 9 px across at thickness 2.
  Auto takes the seven shapes in turn (`markerOffset()` across the
  diagram's graphs), so with auto colors no two of the first 56 curves
  share both. `Diagram::paintLegend()` gives an auto graph a row per
  curve (up to 24, then how many more) - also for auto markers alone -
  with its line and marker, labelled with `curveLabel()`: the swept
  variables' values to four significant digits, their names without the
  graph's own prefix (`r1=2.2k`); the graph's name on an axis is in
  plain ink.
  Only where curves are drawn (Rect, Polar, Smith, ySmith, PS, SP,
  Curve). Saved as an eighth (auto colors) and a ninth (the marker) field
  of the graph line, written only when there is something to say, which
  older versions do not read. The diagram dialog has *auto* next to
  *Color* (the color button off while it is on) and a *Marker* box (off
  for the symbol styles); either in auto switches the legend on; the
  graph list shows them. Covered by `qucs/tests/test_graph_autocolor`
  (the fields, the palette and the shapes, colors and markers per curve
  across graphs, the rendering in pixels - markers on the points of a
  sparse curve and spaced along a dense one - the legend rows, the
  dialog).
- *Done:* **Diagram legend** (#1719). `Diagram::paintLegend()` draws, after
  the graphs and axis texts, a framed white box in the corner chosen by
  `Diagram::legendPos` (off / four corners) with one row per graph: a
  sample of its line in its colour, thickness and dash pattern (or its
  star/circle/arrow symbol) and its variable. It reaches every diagram
  type that uses the base `paintDiagram()` (tabular and timing diagrams
  override it), and the print/export paths, since they paint the same way.
  The position is the 28th field of the diagram's save line, after the
  units and before the quoted labels, read with the same "not a quoted
  label → a newer field" guard the earlier extensions use, so older files
  load unchanged and older Qucs-S ignores the field. The diagram dialog's
  properties tab has a *Legend* box. `qucs/tests/test_diagram_legend`
  checks default/save/load/compatibility, the rendering (pixel colours in
  the chosen corner of an offscreen paint) and the dialog. Along the way
  `DiagramDialog::slotApply()` lost an unchecked `(Schematic*)parent()`
  cast.
- *Done:* **Histogram diagram.** `HistogramDiagram`
  (`qucs/diagrams/histogramdiagram.*`, `<Histogram ...>`) derives from
  the Cartesian diagram and counts every finite value of each graph (a
  complex one by its magnitude) into common bins over the values or the
  x axis' manual limits: `bins` of them, or automatic by
  Freedman-Diaconis (the square root of the number when the quartiles
  meet; 1 to 200, reckoned in doubles), the top edge in the last bin.
  The heights are counts, percentages or densities; the y axis starts at
  0 (a least value of a tenth of the tallest bar puts the automatic
  scale's lower end there). The bars are painted, not traced: new
  `Diagram` hooks paint behind the graphs and over the axis texts, and a
  diagram type saves fields of its own after the common ones
  (`extraSaveFields()`/`loadExtraFields()`: bins, height, the flags for
  the normal fit and the statistics box, the lower and upper limits).
  The statistics box (N, mean, deviation, the share within the limits,
  what fell outside the bins) goes in the top corner the bars reach
  least; the axis labels default to the variables and to what the
  heights are. The diagram dialog has a *Histogram* group, no right axis
  and no logarithmic scales; the zoom rectangle, cursor readout, reset
  limits and free resizing are the Cartesian diagram's. Painting it
  through the export device showed a bug there: a clip set between
  `save()` and `restore()` stayed on, since Qt restores to "no clip" with
  clipping still enabled and the relay turned it back on - every label
  after a clipped drawing went missing from PNG, SVG and PDF exports; the
  relay now keeps clipping off under `NoClip`. The NgMonteCarlo example
  shows its corner frequency in one, with the fit and the spec limits.
  `qucs/tests/test_histogram_diagram` covers the counting, the automatic
  bins, the heights, the fit's room, manual limits, several graphs, the
  settings saved and loaded (and older lines), a schematic, the drawing
  and the dialog; `test_graphics_export` the clip.
- *Done:* **Number notations of a diagram.** A diagram had two:
  "scientific" (`misc::StringNiceNum`: decimal, an exponent when
  |log10| >= 3) and "engineering" (`misc::num2str`: SI prefixes), kept
  as a 0/1 field. `qucs_s::numberformat` (`qucs/numberformat.*`) adds
  decimal (no exponent or prefix, 12 significant digits, or the places
  of the grid step so labels line up), scientific (always an exponent),
  scientific with a power of ten (Unicode superscripts, "10⁶" for a
  mantissa of one), and engineering with an exponent that is a multiple
  of three; automatic and SI prefixes write exactly what they did.
  `Diagram::notation` and `notationDecimals` (places after the point of
  the number or the mantissa, -1 auto) replace `engineeringNotation`;
  the notation is saved in the old field (older versions read 2-5 as
  their "scientific"), the places after the legend position. Every axis
  (rectangular, polar, Smith, 3D, locus curve), the markers (automatic
  keeps the marker's precision as significant digits, the others take
  it as places) and the status bar's cursor readout use
  `Diagram::numberText()`. The dialog lists the notations with examples
  (generated by the formatter) and a *Decimal places* spin box (auto).
  Found on the way: `Diagram::loadGraphData()` returned before laying
  the diagram out when no dataset had changed - with no graphs, always -
  so a loaded diagram kept the labels its constructor computed (0 to 1,
  engineering); it is laid out at least once now (`laidOut`).
  `qucs/tests/test_number_format` covers each notation (automatic and
  SI prefixes against the old functions), places, steps, zero and
  non-finite values, the labels of a diagram in several notations, the
  save/load round trip and older files, the dialog, and the layout after
  loading.
- *Done:* **Line styles that show** (#1723). `Graph::drawLines()` drew a
  graph as `QPainter::drawLines()` of its segments, and Qt starts a dash
  pattern afresh on every line: wherever the points are closer than a
  dash (10 x the pen width) - about every simulation - dashed, dotted
  and long-dashed graphs were solid. The points of every stroke are now
  kept as a `QPolygonF` too (same pass, same cache) and the patterned
  styles are drawn with `drawPolyline()`; solid lines keep `drawLines()`
  and its vertical-run reduction. 16 dashed graphs of 200,000 points
  render as fast as solid ones. On the way, `qucs/tests/test_graph_style`
  showed a graph of two samples drawing nothing at all: in
  `Diagram::calcData()` the "no single point after a stroke end" check
  still used the offsets of upstream's float array (a marker one float,
  a point two) - `p-3` / `p -= 3` in a list of one entry per point meant
  two points, so every stroke of exactly two points was erased: a
  two-sample graph, the first curve of a sweep with two samples a step,
  the last segment of a curve coming back into a diagram with a manual
  range. Its neighbour (`p-2`, the hidden-point case) had been converted.
  The test counts the dashes along a flat line for every style, thickness
  and density, checks a sweep's curves stay apart, and fails on either
  half of the old code.
- *Done:* **Cancel keyboard move restores original position** (#1525).
  A cursor-key move now marks the document modified and is one undo step
  however many key presses it takes (`setChanged(..., 'k')` coalesces
  like marker moves); while it is the latest step, Escape takes it back
  (`Schematic::cancelKeyboardMove()`). A mouse press, another edit, undo
  or redo closes the sequence. Upstream neither recorded the move nor
  marked the document changed, so a keyboard-moved schematic could be
  closed without a save prompt. `qucs/tests/test_keyboard_move` covers it.
- *Done:* **Open any file regardless of current simulator mode** (#1468).
  `Module::registerComponent()` put a component into `Module::Modules`
  (the hash `getComponentFromName()` reads) only when its `Simulator`
  mask covered the simulator in use, so a `.sch` for another backend
  did not resolve: headless it failed to load, and the GUI asked, per
  unknown part, whether to put an empty subcircuit in its place - which
  the next save wrote over the part. `Schematic::rebuild()` (undo, redo)
  reads the document the same way, so an undo after a simulator switch
  lost those parts too. Now every component goes into the hash and only
  the palette is filtered: a component for another simulator is kept in
  `Module::Unlisted` (owned there, freed by `unregisterModules()`) and
  put in no category; of two classes with one model name the first for
  the simulator in use still reads it. `registerModule()` applies the
  same filter to the equation blocks it registers (the *equations*
  group offered the SPICE ones to Qucsator and `.CSPARAM` to Xyce).
  Such a part is drawn with `WrongSimulatorPen`, the ERC reports it as
  "not available for", and the SPICE netlister refuses the run, all as
  before. The `-p` renderer's switch to `simNotSpecified`, added as a
  workaround for this, stays so that a print has no grey parts.
  Over the 247 shipped examples, ngspice refused 30, Xyce 32, SPICE OPUS
  31 and Qucsator 93; now none (but `tunn.sch`, whose device exists only
  after *Load Verilog-A module*). `qucs/tests/test_simulator_modes`
  covers the palette, every component and every example under every
  simulator, the shared model names, the file round trip, the
  application opening one without a question, and undo after a switch;
  five of its cases fail on the old registry.
- *Done:* **Embedded, dockable simulation console** (#235).
  `ExternSimDialog` became `SimulationRun` (`extsimkernels/simulationrun.*`):
  the same netlist → process → output-check → dataset-conversion logic, as
  a plain QObject that holds the schematic in a `QPointer` and writes into
  whatever widgets it is attached to. `SimulationConsole`
  (`qucs/simulationconsole.*`) is the dock — console, status list,
  progress bar, Stop / Save netlist / Clear — tabified with the message
  dock and toggled from *View → Simulation Console*. `QucsApp::
  slotSimulateWithSpice()` asks the console for a run (refused, with a
  status entry, while one is in progress), connects the result handler
  and starts it; the dock is raised for an ordinary simulation and left
  alone for DC-bias display and tuner steps, and shown on a tuner error.
  Since nothing is modal any more: the run is deleted after its result is
  handled, a failed process start ends the run (there is no `finished()`
  after it), `stop()` kills the process and marks the run as having no
  result, a closed schematic stops its run through `destroyed`, and
  `Xyce::killThemAll()` drops its queued netlists so Stop actually stops.
  `misc::simulatorExists()` / `unwrapExePath()` treat an empty path as
  absent (they used to resolve "" to the first directory on `$PATH`, which
  made a missing SPICE OPUS "exist" and could switch the default simulator
  to it). `qucs/tests/test_simulation_console` drives it with a scripted
  simulator (dock shown, app responsive, refusal, Stop, close-while-
  running, failed start) and with the real ngspice when installed.
  The console has two hosts, in three modes: the dock, a window of its
  own, or the legacy window, run modally with `QDialog::exec()` from
  `slotSimulateWithSpice()` once the run was started and stopping the
  run when closed (`QucsSettings.SimulationConsoleHost`, chosen on the
  *Simulation console* tab of the simulator settings);
  `applyHostSetting()` moves the widget between the hosts, run and all,
  and *View → Simulation Console* is one checkable action for whichever
  host is current. The build-message
  dock's own tabs moved to its top so that the two docks' tab bar below
  does not sit under another tab bar.

- *Done:* **Terminal and Python Shell docks.** `ProcessConsole`
  (`qucs/processconsole.*`) is a console around an interactive program:
  `ConsoleProcess` runs it on a pseudo-terminal (`forkpty`, a
  `QSocketNotifier` on the master, `TERM=dumb`; `QProcess` on pipes on
  Windows), the widget shows the byte stream as text through a small
  state machine (escape sequences dropped, `\r` starts the line over,
  `\b` takes a character back), and a line edit with a history sends the
  input. `QucsApp` puts two of them in docks tabified with the simulation
  console, hidden until *View → Terminal* / *Python Shell*, each starting
  its program on first show: `$SHELL -l` (or `/bin/sh`; PowerShell on
  Windows) and `QucsSettings.PythonExecutable` (*Application Settings →
  Locations → Python Path*) or `python3` on `PATH`, with
  `PYTHON_BASIC_REPL=1`. The program goes with the console (hang-up,
  then kill; `waitpid` so no zombie is left).
  `qucs/tests/test_process_console` drives `/bin/sh` and `python3`.
- *Done:* **Claude Code dock** (`qucs/claudecode.*`, `qucs/claudecodepanel.*`).
  Claude Code's own interface is a full-screen terminal program, which the
  console above does not emulate; the dock drives it headless instead:
  `claude -p --input-format stream-json --output-format stream-json
  --verbose --include-partial-messages --permission-prompt-tool stdio`
  (plus `--permission-mode`, `--model`, `--resume`, and
  `--append-system-prompt` telling Claude where it runs), one process for
  the conversation, a JSON line per prompt. `qucs_s::claude::Session`
  parses the stream - `system/init` (session, model), `stream_event` text
  deltas, whole `assistant` blocks (text, `tool_use`), `user` tool
  results, `control_request`/`can_use_tool` (answered with a
  `control_response` allow/deny, optionally `updatedPermissions` setMode
  acceptEdits for the session), `result` (time, cost, steps, denials) -
  into states (thinking, running a tool, waiting for permission, ready,
  failed) and signals; an interrupt is a `control_request` and the program
  goes if the turn does not end. A stopped program, or another mode or
  model, starts again with `--resume` at the next prompt; another folder
  starts a new conversation. `ClaudeCodePanel` renders the conversation
  in a `QTextBrowser` (prompts on a tint, replies as Markdown with code
  shaded, a line per tool with its mark, a summary per turn), asks for
  permissions in a card (*Allow*, *Allow All Edits*, *Deny*), and has a
  composer (Enter sends, the document in front attached on request), a
  folder row and a menu (permission mode, model, folder, program). It
  works in `QucsSettings.qucsWorkspaceDir` and follows it when the setting
  changes. `QucsApp::reloadChangedFiles()` loads again the documents
  Claude wrote or edited when they have no unsaved changes, and a status
  bar chip (`statusClaude`) shows the session's state and toggles the
  dock. The program is found by the setting, `PATH`, then its installers'
  places (`findProgram()`); `QUCS_CLAUDE` overrides, and the tests point it
  nowhere. The models in the menu are the program's own:
  `qucs_s::claude::ModelQuery` starts it once when the dock is first shown,
  sends the `control_request` `initialize` its SDKs send and closes its
  input, so it answers (its `models`: value, resolved model, description,
  `supportsAutoMode`) and ends without a model call (under a second);
  `modelChoices()` names them from their descriptions, adds the newest of
  each family the program does not list (Opus 5.5 by its full name - the
  aliases stand for what that version thought newest) and the settings keep
  the list for the next start. *Auto* permission mode (`--permission-mode
  auto`) is offered where the model has it; the program falls back to
  asking otherwise and says so only in `system/init`'s `permissionMode`,
  which the session compares with what it asked for and reports once. The
  protocol was checked against Claude Code 2.1.267 (which answers Opus 5.5
  with "version 2.1.280 or newer is required", shown as the turn's
  failure); `qucs/tests/test_claude_code` drives the session, the model
  query and the dock with shell scripts that answer as claude does.
  Tools in a row are one line that opens (`toggle:group:<id>` and
  `toggle:tool:<id>` anchors in the transcript; the session passes each
  tool's input - the whole command, the edit - with `toolStarted`, and
  the panel keeps the first 30 lines of what it gave).
  `ClaudeCodeTabs` (`qucs/claudecodetabs.*`) holds the conversations, a
  `ClaudeCodePanel` and a session each: New in a panel's header asks it
  for a tab (`setNewInTab`), a new one works in the folder of the one it
  was opened from, a permission request brings its tab forward only when
  the dock is hidden or it is in front (else the tab's mark and the
  status bar say so, and the chip leads to it), notes about reloaded
  files go to the conversation that changed them, and the status bar
  reports `mostUrgent()` with a count of the others at work.
- *Done:* **Claude drives the Qucs-S window** (`qucs/qucscontrol.*`).
  The dock's conversation could say which document was open and reload
  files Claude wrote, but Claude had no way into the GUI. Qucs-S is now an
  MCP server for it, without a socket or a helper process: the claude
  program is told of an "sdk" server (`--mcp-config
  {"mcpServers":{"qucs":{"type":"sdk","name":"qucs"}}}`, as the Agent SDKs
  do), the session announces it in its `initialize` control request, and
  the program sends the server's JSON-RPC as `control_request`s of subtype
  `mcp_message`; `Session::handleMcpMessage()` answers initialize (with
  the server's instructions), notifications, ping, tools/list and
  tools/call (from the event loop, not the output's reader, since a tool
  may run a dialog's event loop) through a `ToolHost`. `QucsControl` is
  that host: 23 tools - get_state; open, new, show, save (as), close
  documents; get_schematic (a summary with each pin's place and net -
  a label's name, gnd, net1, ... - and the nets with their pins, or the
  .sch text: `Schematic::documentText()`), set_schematic (`Schematic::replaceContent()`: the
  sections given in place of the document's, through the undo record's
  rebuild, restored when they do not read), add_component
  (`Module::getComponent()`, placed as a click places one), edit_component
  (properties through recreateComponent(); turned and moved keeping the
  circuit - off its nodes, the wires of other nets under its pins' new
  places taken up, put down, every net then in pieces joined again, the
  nets through wires, labels and ground compared before and after, the
  edit undone with the reason when they differ), delete, connect (by a
  route that runs over nothing of another net - a wire joins every node
  it runs over - around the parts first; it joins its ends' nets and
  nothing else, or draws nothing and says why) and add_wire (not drawn
  over another pin), set_label, select, zoom, undo, redo;
  screenshot (graphicsexport's image, or the viewport's; an MCP image
  content Claude sees); list_component_types; list_actions and
  trigger_action (the menu bar walked; triggered from the event loop and
  answered half a second later with the dialog it opened, if any; quitting
  and the actions that open the system's file and print dialogs refused);
  get_dialog and set_dialog (the controls of the open dialog - on every
  tab, labelled from forms, buddies, their layout's row - set as a user
  would, the tab brought forward, a button pressed from the event loop);
  simulate (Simulation > Simulate from the event loop, the SimulationRun
  watched to its end, the dataset's time checked). Each change is one
  `setChanged(true, true)`, the document comes to the front and the view
  follows what changed. What Qucs-S would put in a message box while a
  tool runs is told to Claude instead (`misc::ErrorCapture`, which also
  makes a file's unknown component an error rather than a question).
  Permissions: the tools that look are `--allowedTools`; the others ask,
  with *Allow Qucs-S Control* for the rest of the conversation
  (`PermissionRequest::canAllowTools`; the session then answers them
  itself, as it does in the Accept Edits, Auto and Bypass modes).
  `QucsApp::saveDocumentAs()` is Save As without its dialogs. Checked with
  Claude Code 2.1.267 and Haiku: it found the tools with ToolSearch, placed
  R1, V1 and a ground, wired them, took a screenshot and described the
  circuit (16 s, $0.12). The GUI monkey's tool calls found products of
  far-away coordinates overflowing int in `geom::is_it_line()`, now
  computed in 64 bits. A user's session found edit_component turning a
  part whose ports pointed at nodes `detachComp()` had deleted, and a
  real Claude run checked net by net found connect's routes shorting the
  circuit it built (see the stress hunt's findings); the router and the
  net checks came of them, `Schematic::snapshot()` and `restore()` being
  what a route found wrong is taken back with.
- *Done:* **TeX math in the Claude Code dock** (`qucs/mathtypeset.*`).
  QTextDocument's Markdown has no math, and a web engine for KaTeX would
  be a heavy dependency for a panel, so the math is typeset here: a
  recursive parser (amsmath's commands, environments, siunitx units,
  `\left`/`\right`, `\not`, fonts and alphabets; what it does not know
  it shows as written, and nesting is bounded) builds boxes laid out as
  TeX does - four styles, the atom classes and the space between them,
  fractions and scripts by TeX's shifts around the math axis, limits
  above and below in display style, delimiters drawn to height (glyphs
  when small), radicals, accents, matrices and aligned rows - painted
  with STIX Two (or Cambria, Latin Modern, Times) at the text's x-height.
  `findMath()` takes `$...$`, `$$...$$`, `\(...\)`, `\[...\]` out of the
  Markdown before it is read (not in code, and not money: a `$` opens
  before a non-space and closes after one, not before a digit), and the
  formulas go back as `MathObject`s, a text object of its own: Qt's
  image handler rounds an image to whole pixels and scales it by the
  screen's dots per inch over 96, which on macOS shrank the math and
  moved it off the baseline. With `AlignMiddle` and padding computed
  from the text's x-height, the math's baseline is the line's. The
  system prompt tells Claude that math between dollars is typeset.
- *Done:* **A Claude Code conversation exported** (`qucs/claudecodepanel.*`).
  *⋯ → Export Conversation → PDF / Markdown / Plain Text*, from the
  panel's entries rather than from what the dock shows (where the tools
  are folded): a header (the first prompt as the title, when, the
  folder, the model, the Claude Code version, the session to resume),
  then every prompt, reply, tool - its input and what it gave, as when
  opened - note, problem and turn summary. Markdown keeps the replies as
  Claude wrote them (math between dollars as it was), a prompt as a
  quote, the tools as a list with their input and output in code
  fences longer than any backtick run in them. Plain text reads each
  reply's Markdown with QTextDocument and writes its blocks back out:
  list marks, quotes, code indented, a table a row to a line, the math
  as TeX. The PDF is the dock's own drawing (`renderConversation()`)
  with the colours of paper - dark on white whatever the theme - every
  tool open, no links, math typeset at four times the resolution; the
  document laid out as on the screen is paginated with
  `setPageSize()`, painted page by page onto a QPdfWriter scaled from
  the screen's dots per inch to 300, with the title and "n of N" in a
  footer (A4, or Letter where the measurement system is American).
- *Done:* **Login-shell environment at start** (`qucs/shellenvironment.*`).
  `importLoginShellEnvironment()` runs `$SHELL -l -i -c "printf marker;
  exec env -0"` (stdin from /dev/null, killed at a timeout; then `-l`
  alone if that said nothing), parses the NUL-separated dump after the
  marker (a profile's greeting cannot corrupt it), and `qputenv`s what
  the process lacks: PATH merged (`mergedPath()`: the process's own
  directories first, then the shell's in order), shell-session names
  skipped, existing values untouched. Called in `main()` before the GUI
  starts (the CLI modes are left alone), off with `QUCS_NO_SHELL_ENV`.
  `qucs/tests/test_shell_environment` covers the parsing, the merge, a
  scripted shell, a hanging one and the real ones.
- *Done:* **Content panel refreshes by itself.** `ProjectView` polls: a
  repeating timer (`QucsSettings.ContentRefreshSeconds`, off with
  `ContentAutoRefresh`, both under *Application Settings → Settings*)
  compares `listingSignature()` - every project file with size and
  mtime - with that of the listing shown and rebuilds only on a
  difference, never while a simulation runs, a popup is open or a drag
  is going. (A first version watched the directories with a
  `QFileSystemWatcher` and re-pointed it after each refresh; on the
  user's machine that refreshed without end and made the panel
  unusable, so the change check and the user-set interval replaced
  it.) A *Refresh* action sits on the empty area's menu only. Every
  file row now has its note cell, empty or not: a row short
  of a cell in the two-column model had Qt's accessible-table layer
  asking for the missing cell, losing count of the rows and crashing on
  the next expand while an assistive client was attached.
- *Done:* **Editor panes (up to 2x2).** The central widget is a vertical
  `QSplitter` of row `QSplitter`s of `PaneWidget`s (a marker bar over a
  `ContextMenuTabWidget`); `QucsApp::DocumentTab` is the *active* pane,
  so the ~150 existing uses of it keep working on the pane where the
  user is, and `qucs_panes.cpp` adds what must span panes: `panes()`,
  `paneOf()`, `allDocuments()`, `setActivePane()` (called from
  `QApplication::focusChanged`, a press on a pane's tab bar, the tab
  context menu, a drop, and `gotoPage()` when the file is open in
  another pane). `findDoc()`, `closeAllFiles()`, `slotFileSaveAll()`,
  `autosaveAll()` and the modified-marker update walk every pane; a new
  pane starts with the usual untitled placeholder, which goes when a
  document arrives (`dropPlaceholder()`), and a pane whose last document
  closes is removed (`closeFile()`, `moveDocument()`). Actions under
  *View → Panes* and shortcuts through the shortcut manager
  (`View.SplitRight` …). `qucs/tests/test_panes` covers the grid, where
  documents open, moves, closes, focus/click activation, drops and the
  spanning operations.
- *Done:* **Theme: System, Dark, Light** (`qucs/apptheme.*`, setting
  `Theme`, *Application Settings → Appearance*). `apply()` asks the
  platform through `QStyleHints::setColorScheme()` (Qt 6.8+; Cocoa and
  Windows answer, and the native controls follow) and, when
  `colorScheme()` does not come back as requested (Linux, the offscreen
  platform), puts a spelled-out dark or light palette on the
  application. *System* is `unsetColorScheme()` plus a palette with an
  empty resolve mask, which makes Qt take every role from the platform
  theme again — now and on later system changes — instead of restoring
  a copy taken at start. Applied in `main()` after the saved style and
  from the dialog's *Apply* (also when only the style changed, since a
  style brings a palette of its own); `misc::isDarkTheme()` now reads
  the application palette instead of leaking a `QLabel`.
  `qucs/tests/test_app_theme` covers the bounds, both palettes, the
  round trip to the system's look (resolve mask 0, no `AA_SetPalette`),
  the platform-vs-palette decision, storage, and the dialog's combo
  (choice, save, the main window following, *Default Values*).
- *Done:* **Designed themes.** Ten themes that look the same on every
  platform, next to the platform's System/Dark/Light: Daylight, Paper,
  Solarized Light, Catppuccin Latte, Graphite, Nord, Dracula, One Dark,
  Solarized Dark, Catppuccin Mocha (`apptheme::designed()`, ids 3-12 of
  the same `Theme` setting). Each is a set of `Colours` - window,
  surface, base, alternate, raised, border, text, muted, disabled,
  accent and the text on it, link, and the schematic's paper and grid -
  made into a palette (`palette()`) and a style sheet for the tool bars,
  docks, tab bars, menus, scroll bars, headers, group boxes and status
  bar (`styleSheet()`). They draw with `DesignedStyle`, a `QProxyStyle`
  over Fusion that paints buttons, fields, check boxes and radio buttons
  flat and rounded from the palette alone - so the colour-picker buttons,
  whose palette carries the colour, keep showing it; no style sheet
  touches them - and inks the icons of tool bars on a dark palette.
  `apply()` swaps the style in and out and remembers the platform's
  (`nativeStyle()`, `setNativeStyle()`: *App Style* chosen under a
  designed theme waits for a platform theme), and sets the platform's
  colour scheme to the theme's darkness for the window frames and native
  dialogs. `QucsApp::applyTheme()` / `applyLook()` bring the component
  list (its colours from the palette; `InkedIconDelegate` inks its icons
  on a dark list), the paper, open text editors and *View → Theme* (a
  radio menu with a swatch per theme) in line. `ink::inked()` inks a
  pixmap or an icon pixel by pixel through `on()`. The paper setting
  became *Schematic paper and grid from the theme*: `misc::paperColor()`
  and the new `misc::gridColor()` take a designed theme's own; the
  platform's Dark keeps the dark paper. The text editor takes a designed
  theme's base and text, its syntax colours fitted with `ink::on()`, the
  current line and the line-number margin from the theme; the
  highlighter's `setLanguage()` no longer piles up rules when called
  again. The status bar's warning label no longer blinks back to black
  (it takes the status bar's colour). `test_app_theme` checks every
  theme's contrasts (text 4.5:1 on all its backgrounds, muted text 3:1,
  the accent's text 4.5:1, disabled text dim but there, the grid on the
  paper), its palette and style sheet, the style's swap and the waiting
  App Style, the paper and grid, a picker button and a checked box as
  drawn, the dialog's combo and *View → Theme*, the component list
  (no dark-blue pixel left on Nord), the text editor and the warning
  label; `QUCS_TEST_GRAB=<dir>` saves every theme's main window, menu,
  editor, side panel and settings. `test_ink` covers `inked()`.

- *Done:* **A status bar worth reading.** It held the cursor position, a
  "no warnings" label that blinked red after a run with warnings, and the
  diagram readout as `X=...; Y1=...`, while "Ready." messages without a
  timeout covered its left side for good. `StatusPanel` (statusbar.h)
  owns it now: a `HintLabel` on the left (elided, whole in its tool tip)
  with the hint of the mouse mode in hand (`modeHint()`: from
  `MousePressAction`/`MouseMoveAction`, the element being placed and the
  wire planner's route), and a `ChipRow` on the right that shows each
  chip while it is wanted and there is room for it and for every more
  important one (run, problems, position, readout, zoom, selection,
  simulator, saved, grid, theme), with a minimum width of 0, so it never
  holds the window wide. The problems chip runs `erc::check()` on the
  schematic in front 400 ms after `Schematic::signalEdited()` (the new
  signal of `setChanged(true)`) and again when the simulator changes;
  the run chip follows a `SimulationRun` to `simulated` (`watchRun()`)
  or Qucsator's run (`runStarted()`/`runEnded()`); the simulator chip
  asks the program for its version once the window shows
  (`versionArguments()`, `versionIn()`: `--version` or `-v`, stdin
  closed, killed after 3 s, kept per program and date); the saved chip
  reads the file's date and `autosave::writtenAt()`. The panel refreshes
  on the next turn of the loop after the document in front is painted
  (an event filter on its viewport), edited or switched, a tool is
  taken, a run moves or the theme changes; a clock ticks the run's
  seconds and the ages. `status::readout()` names each axis by its
  variable without its dataset, with the unit its name tells
  (`unitOf()`) or the axis' dB scale, and `withUnit()` writes SI
  prefixes to four digits; a diagram with a notation of its own keeps
  it; a histogram reads as its variable and the count (percent,
  density) of its bar. `SimulationRun` counts warning lines (`countWarnings()`) and knows
  when it was stopped. The chips are flat in every style (a style sheet
  on the row from the palette; the macOS style would bevel them) and
  share one size of type, 11 points at most. The "Ready." messages
  became `clearMessage()`, and "Saving aborted" is no longer overwritten
  at once. `test_status_bar` covers units, prefixes, the readout, ages,
  durations, versions and warning counts, and in the window the hint per
  tool, cursor and readout, the selection, grid and zoom (its menu), the
  problems after an edit, the run chip through its states, the
  simulator's version from a stand-in program and the switch, the saved
  state through autosave and a save, the theme chip's menu, the marks'
  contrast on every designed theme and the row giving way;
  `QUCS_TEST_GRAB=<dir>` saves the bar in six themes.

- *Done:* **A grid setting for every schematic.** The grid's visibility
  was only per document (`QucsDoc::a_GridOn`, the third field of the
  file's `<Grid=...>`, toggled by *View > Show Grid (current document)*
  and *Document Settings*). `QucsSettings.GridMode` (0 as each says, 1
  always hidden, 2 always shown; *Appearance > Schematic grid*) and
  `Schematic::gridShown()`, which `drawGrid()` asks, override it for
  drawing only - the flag and the file stay as they are, snapping is
  unchanged, and a data display (`.dpl`) keeps its own.
  `QucsApp::applyGridSetting()` redraws every open schematic;
  `updateGridAction()` makes *Show Grid* read "(all schematics)" and
  turn the setting over (saved at once) while it overrides, and act on
  the document otherwise; *Document Settings* says when its box is
  overridden. `qucs/tests/test_grid_setting` covers what is drawn in
  each mode (pixels of an empty canvas), data displays, the saved file,
  the action in both roles and the settings dialog; `test_app_theme`'s
  combo lookup no longer depends on the order of the dialog's combos.

- *Done:* **Switch Workspace, Import Project, Link Project** (the
  Projects panel's context menu, and the Project menu).
  `qucs_s::workspace` (`qucs/workspace.*`) is the file side, free of
  widgets: `check()` (a folder named `*_prj`, a name ending in `_prj`
  without separators, not in the workspace already - by its place or as
  the link there -, not holding the workspace, nothing of that name
  there yet), `importProject()` (`std::filesystem::copy`, recursive,
  links inside as links; a failed copy is removed), `linkProject()` (a
  directory symbolic link; on Windows, where that needs Developer Mode or
  an administrator, a junction via `mklink /J`), `freeName()`
  (`amp_2_prj`), `isLink()`/`linkTarget()` (symbolic links and
  junctions) and `removeLink()` (unlink / `RemoveDirectory`, never
  following). `QucsApp::switchWorkspace()` closes the documents (asking
  about unsaved ones), `setWorkspace()` sets the workspace *and* the
  folder the panel lists (`projsDir`), closes the old workspace's
  project and saves the setting; the settings dialog now goes through it
  as well - it set only the workspace, and the panel went on listing the
  old one. `bringProjectIn()` asks for another name when the workspace
  has one of that name, and selects the project in the panel once the
  model has read the folder. A linked project is listed in italics with
  "Linked from ..." as the tooltip. `deleteProject()` removes a linked
  project's link and nothing else: upstream's `QDir::removeRecursively()`
  on the link emptied the original project through it and then failed on
  the link (checked by running the test with the guard disabled).
  `qucs/tests/test_workspace_projects` covers the copy (subfolders,
  hidden files, links), every refusal, the link (one project from two
  places), removing a link, the menus, switching (a folder that does not
  exist yet, the setting saved, the open project closed), import and
  link from the application (selection, italics, tooltip, the name
  asked for, cancelling, a message), deleting a linked project, a linked
  project opened, listed, saved and given its Scratch folder in its real
  place, and the settings dialog's switch.
  Found in use on macOS: the folder dialog hands a folder over as
  `…/project1_prj/`, and `QFileInfo` takes the name after that slash as
  empty - every import and link was refused as "not a project". Worse,
  read through the slash a linked project is the folder it leads to, so
  *Project → Delete Project* on one asked to destroy the project's files
  and, answered Yes, emptied the original (then failed on the link).
  `workspace::check/importProject/linkProject/isLink/linkTarget/
  removeLink`, `bringProjectIn()`, `openProject()` and `deleteProject()`
  clean the path first (`QDir::cleanPath`), and `recurRemove()` refuses
  a link whatever it is given. Covered: import, link and the checks
  with `/`, `//` and `./` in the paths, from the application too, and a
  linked project deleted as the dialog gives it - the link goes, the
  project stays.

**Medium (weeks each)**

- *Done:* **Auto-placement of DC-bias labels** to avoid overlaps (#1692).
  `Schematic::drawDcBiasPoints()` drew each value in a box at a fixed
  offset from its node (`SweepDialog::setBiasPoints()` worked out
  "no room to the right" and "horizontal wire" flags that nothing
  read). `qucs_s::bias` (`qucs/biaslabels.*`) is the placement, free of
  widgets as the issue asked: `candidates()` - the four corners beside
  the anchor (upper left first for a voltage, upper right for a
  current: the old offsets), the four sides, then the same eight 24
  units out; `cost()` - boxes by area, wires by the length inside the
  box (Liang-Barsky; along an edge is free) times its height; and
  `place()`, which orders the labels by how few free places they have,
  then gives each the candidate covering the least of the labels
  placed so far and, among those, the least of the rest, a step out
  costing half the label's area and getting a leader line.
  `Schematic::layoutBiasLabels()` gathers the obstacles (symbols, their
  property text, node dots, wire and node labels, wires, diagrams,
  paintings) and `drawDcBiasPoints()` draws the leaders, then the boxes.
  Which nodes get a value, the values, units, colours and boxes are
  unchanged. `qucs/tests/test_bias_labels` covers free placement, a
  symbol, a wire, labels close together, the hardest label first, the
  fallbacks, the candidates, a survey of the shipped examples (every
  value shown as `setBiasPoints()` picks them: label on label 436 -> 2,
  on a symbol or its text 4135 -> 2200, crossed by a wire 3950 -> 478,
  no schematic worse) and the drawing. A real ngspice DC bias of
  `audio_amp.sch` (94 labels) was checked by eye.
- *Done:* **Device operating points** (#789, where dumping `@D1[cd]`
  and the like was asked for). The DC bias run (`a_DC_OP_only`) now
  ends with `show all > spice4qucs.cir.dc_op_dev` after the node values
  (ngspice only; SPICE OPUS shares the kernel class). `qucs_s::oppoint`
  (`qucs/oppoint.*`) reads it - a block per device type ("BJT: ...",
  an OSDI module's name), a `device` row naming up to three devices, a
  `model` row, a row per parameter; "-" and rows of the wrong width are
  left out - and gives each device to its component by the netlist's
  naming (the name, or the SPICE letter and the name: `T1` -> `mt1`,
  `Pr1` -> `vpr1`, `SUB1` -> `xsub1`; `m.xsub1.m9` is M9 inside SUB1).
  `unitOf()` knows the quantities by name and first letter (A, V, S, F,
  C, W, Ohm; `*mod` flags, `mult_*`, geometry and counts have none),
  `isOperatingQuantity()` separates them from the set-up. The device
  file is read only if it is not older than the node file of the same
  run (the Scratch folder keeps an earlier run's). `Schematic` keeps the
  devices; its viewport's tooltip, while the DC bias is shown, is the
  operating point of the component under the mouse (nonzero operating
  quantities, 24 rows at most, rounding zeros like an off transistor's
  1e-134 A left out). The message dock's *Operating Point* tab
  (*View -> Operating Point*) lists all of it: components with a
  semiconductor or OSDI device first, then the circuit elements, in
  name order counting numbers; a filter that keeps what matches and
  what is above or below it; *Copy* as tab-separated
  component/device/parameter/value/unit; a click selects and centres
  the component. `qucs/tests/test_operating_point` covers the parser on
  ngspice 46 output (BJT, diode, Mos1 with a subcircuit device, resistor,
  sources, an OSDI module), the attribution, units, values, the tooltip,
  the schematic and the tab, and a real DC bias run of two shipped
  examples when ngspice is installed (it is on the Linux CI job).
- *Done:* **Optimization with ngspice** (#1327). The `.Opt` component
  was qucsator-only: ASCO drives qucsator, and with ngspice the
  component was not in the palette, and a schematic that had one was
  refused as "SPICE-incompatible". Whether ASCO can drive ngspice
  netlists with a `.control` section - how Qucs-S runs every analysis -
  is open upstream (untested here: ASCO is not installed), and stock
  ngspice has no optimizer of its own, so Qucs-S searches itself. `qucs_s::optimization` (`qucs/optimization.*`)
  reads the component as the dialog writes it (variables
  `name|yes|initial|min|max|type`, goals `name|MIN/MAX/LE/GE/EQ/MON|value`,
  the DE settings), knows the E series (the standard's tables, 9.19 of
  E192 included), reads a Qucs dataset, finds a goal among its variables
  (`gain` is `ac.gain`; `vo` is `ac.v(vo)`, as ngspice keeps an
  expression of a voltage one), measures a sweep by its worst point for
  the goal, and weighs the goals into a cost (objectives relative to
  the start, constraints' shortfall relative to the target, the
  component's weights). `DifferentialEvolution` is Storn and Price's,
  the ten strategies of the dialog (whose list named DE/rand/1/exp
  twice; the fifth is DE/rand/2/exp), asked for candidates and told
  their costs, seeded as the component says. `NetlistScope` makes the
  schematic netlist one candidate: the variables' values in their
  definitions (equation, `.PARAM`, `.GLOBAL_PARAM`), a `.PARAM` line
  for a variable only a component's value names
  (`AbstractSpiceKernel::setExtraParameters`), only the optimized
  simulation, and everything back afterwards. `Optimizer`
  (`extsimkernels/optimizer.*`) simulates the start (a goal the results
  lack ends the run with the names they have), then each generation's
  candidates in `Scratch/<schematic>/opt/1..8`, up to eight ngspice
  processes at a time; it stops after the generations, at convergence
  (the population's cost variance), or when every goal is a limit and
  all are met. `SimulationRun::start()` runs it for an active `.Opt`
  with ngspice (not for DC bias or a tuner step), writes the best
  values into the component's variables (undoable, as ASCO's are) and
  simulates the best point with every simulation for the diagrams;
  *Stop* stops the search and keeps the best. The ERC reads the
  component too. Example: `examples/ngspice/NGspice features/
  LC_lowpass_optimization.sch`. `qucs/tests/test_optimization` covers
  the component's formats, the series, the dataset, goal names and
  sweeps, the cost, all ten strategies on a sphere (bounds, the same
  seed the same search), the netlist of a candidate and the schematic
  after it, the ERC - and, with ngspice, a divider tuned to a ratio
  (an equation's variable on a log scale; a resistor's only, from E24),
  a missing goal, a missing simulation, a stopped run, a start that
  meets its goals, the example, and DC bias beside the component.
- *Done:* **NgOpt: ngspice's own `optimize` as a component.** ngspice
  builds from Ngspice_OpenVAF_Enhancements have a parameter optimizer
  (`optimize`, its E-130/143/144/145/194/195/196): knobs of three kinds
  (`-dparam` a `.param`, re-sourced; `-param` an instance, `alter`;
  `-mparam` a model parameter, `altermod`), a scalar `-minimize` after
  one analysis or least-squares `-target`s over up to eight `-analysis`
  stages, and the methods nm, lm, pso, de and sa - run inside one
  ngspice process, with no netlist written per candidate. The NgOpt
  component (*simulations*, ngspice only, `.NGOPT`) is that command:
  `qucs_s::ngopt` (`qucs/ngoptimize.*`) keeps it in the component's
  properties (`Method`, `MaxIter`, `Tol`, `Size`, `Seed`, `Verbose`,
  `Analysis`, `Minimize`, then a `Knob=kind|name|init|lo|hi` or
  `Target=analysis|expression|value|weight` each, saved with their names
  like the Optimization component's), writes the line (values with Qucs
  prefixes as numbers, an expression as one token, a stage per analysis
  in the order they come, an analysis named after a simulation component
  taking its command), and reads what ngspice prints at the end.
  `Ngspice::createNetlist` puts the line first in `.control`: ngspice
  leaves the circuit at the optimum, so the schematic's simulations show
  it. The netlist resets after each simulation, which keeps a `.param`
  the optimizer changed (the deck is edited) but not an `alter` or an
  `altermod`: the in-place knobs' values (`optimize_<name>`) go into
  shell variables right after `optimize` and are set again after every
  `reset`. `SimulationRun` puts ngspice's verdict (converged or
  interrupted, the cost, the evaluations, its NOTEs) in the status log
  and the values found into the knobs' initial values (undoable); an
  ngspice without the command ("no such command") is said to be one.
  `NgOptDialog`: the method (with what it is good for) and its settings,
  a table of knobs (*Add Equation Variables* makes one of every number
  an equation or `.PARAM` defines, a decade each way), the objective -
  an expression after an analysis, or a table of targets - and the
  command it makes, live. The ERC reports a command that cannot be
  written. Example: `examples/ngspice/NGspice features/
  LC_lowpass_ngopt.sch` fits the low-pass to a 1 MHz Butterworth
  response by least squares (3.183 nF, 15.92 uH, 3.183 nF, 39
  evaluations). `qucs/tests/test_ngopt` covers the command line and what
  it refuses, the carried knobs, ngspice's output, the component saved
  and loaded, the netlist (order, resets, a refused command, an inactive
  one), the dialog, the ERC, an ngspice without the command - and, with
  one that has it, the example and a divider whose instance knob is
  still at the optimum in the transient after the AC analysis.
- *Done:* **NgMonteCarlo and NgCorners: ngspice's own `montecarlo` and
  `corners` as components.** The same ngspice builds have a packaged
  Monte Carlo (`montecarlo N -analysis ... -expr name=... -spec ...
  -min/-max`, its E-151/552 and the `-lhs`/`-seed` policies of
  E-535..538) and a corner loop over the corners Verilog-A models
  declare (`corners [-list] [-nonominal] -analysis ... -output ...` or
  `-mc N ...` per corner, E-654/655). The two components (*simulations*,
  ngspice only, `.NGMONTECARLO`, `.NGCORNERS`) are those commands:
  `qucs_s::ngstats` (`qucs/ngstatistics.*`) keeps them in the components'
  properties (`Samples`, `Seed`, `LHS`, `ModelStats`, `Analysis`; for
  corners also `Corners`, `Nominal`, `Waveforms`; then a
  `Record=name|expression` or `Spec=metric|min|max` each), writes the
  lines (a spec's metric is recorded too, as `spec1`, `spec2`, ...) and
  the `.control` block `Ngspice::createNetlist` puts after the
  simulations: between two echoed markers, a transient interpolated
  onto its step (`option interp`) so every sample or corner has the same
  points, `option osdimc` around the run for the models' statistics, the
  `montecarlo<n>` or `corners<n>` plot written as ASCII into the Scratch
  folder, and for corners each corner's own analysis plot - kept by the
  command before its plot, walked back with `setplot previous` - appended
  into a second file. A small raw reader (`dims=`, padded or not,
  complex, several plots in a file) turns them into the dataset under
  the component's name: `sample` or `corner` as the scale, a scalar per
  sample, a waveform family against the analysis scale
  and the sample or the corner, the yield and counts. `SimulationRun`
  puts ngspice's report (samples, yield, confidence interval,
  violations, notes; the corners with their values) in the status log,
  and says when an ngspice has no such command. `NgStatisticsDialog`:
  the analysis, samples, seed, sampling and statistics, or the corners,
  the nominal, the waveforms and a Monte Carlo per corner; tables of
  values and specs; the command it makes, live. The ERC reports a
  command that cannot be written. Example: `examples/ngspice/NGspice
  features/RC_lowpass_montecarlo.sch`. `qucs/tests/test_ngstats` covers
  the command lines and what they refuse, the components saved and
  loaded, the raw reader, the dataset, the netlist, the log, the dialog,
  the ERC, an ngspice without the commands - and, with one that has
  them, the example (200 samples, the family, the yield, and the
  example's Histogram diagram with the share between its limits equal
  to ngspice's yield)
  and the corners of a compiled Verilog-A resistor, their values and
  waveforms and a Monte Carlo at two of them.
- *Done:* **NgSweep: ngspice's own `sweep` as a component.** The same
  ngspice builds have a universal parametric sweep (E-146: `sweep <knob>
  (lin N a b | list v...) [-vs <knob> <spec>]... -analysis <cmd> -output
  ...`; the knob kind - `alter`, `altermod`, `.param` - found by ngspice;
  E-190's outer knobs; each run's analysis plot kept before the sweep's
  own). Qucs's parameter sweep (`.SW`) writes its own `foreach` loop and
  does not use it. The component (*simulations*, ngspice only,
  `.NGSWEEP`, `isSimulation` so an `op` sweep needs no other simulation)
  is that command: `qucs_s::ngsweep` (`qucs/ngsweep.*`) keeps it in the
  properties `Analysis`, `Param`, `Type` (lin, log, list), `Start`,
  `Stop`, `Points`, `List` (not `Values`, which the component dialog
  reserves), `Waveforms`, then `Vs=knob|type|start|stop|points|list` and
  `Record=name|expression`; writes the line - a log sweep as the list
  of its points, as ngspice's `dec` counts per decade; the knob values
  are the ones ngspice computes - and the `.control` block after the
  simulations: between markers, `destroy all` so the runs' plots are
  the only ones before the sweep's, `option interp` for a transient,
  the sweep with the records and the saved voltages and currents as
  `-output` (every analysis but `op` needs one), the sweep plot written
  as ASCII, then each run's plot, walked back to with `setplot
  previous`, appended into a second file. The dataset: the knobs'
  values (the inner knob's from the sweep plot), every run's waveforms
  as a family against the analysis scale and the knobs (resampled onto
  the first run's scale if a run's points differ), the recorded values
  against the knobs - with outer knobs found by ngspice's own names for
  the curves (`peak_c1_1e_07`), since `write` puts the vectors in
  alphabetical order; after an `op` or when the waveforms are missing,
  the voltages' and currents' last values. `SimulationRun` puts what was
  swept and ngspice's warnings in the status log, says when a run's
  waveforms could not be read or the ngspice has no `sweep`, and leaves
  out of the warning count the `checkvalid` lines ngspice prints of the
  knob when it reads it to tell its kind. `NgSweepDialog`: the analysis,
  the parameter (the schematic's components, equation variables,
  `temp`), the sweep, outer sweeps, recorded values, and the command,
  live; the ERC reports a command that cannot be written. The analysis
  of a named simulation component is now taken from it switched off too
  (`ngopt::analysisCommand`), for NgOpt, NgMonteCarlo and NgCorners as
  well: off, it runs only in the command that names it. Example:
  `examples/ngspice/NGspice features/RC_lowpass_ngsweep.sch`.
  `qucs/tests/test_ngsweep` covers the knob values and specs, the
  command line and what it refuses, saving and loading, the block, the
  dataset (families, outer knobs by name, a missing run, an `op`, a
  transient resampled), the log, the netlist, the ERC, the dialog, an
  ngspice without the command - and, with one that has it, an ac sweep
  against the analytic low-pass at every point, an `op` over two knobs,
  a transient, and the example with its diagram in auto colors and
  markers.
- *Done (click, not hover):* **Net highlighting.** `Schematic::netOf(Wire*)`
  flood-fills the `Node`↔`Wire` graph and, in rounds, joins what labels
  of the same name and ground symbols connect; `selectedNet()` is the
  union over the selected wires, and `drawElements()` paints it as a
  translucent orange glow under the wires and nodes (not in symbol mode,
  and `paintSchToViewpainter()` - exports, prints - never). Computed per
  repaint (a few hundred wires at most), so no selection hook is needed.
  Hover stays out until the canvas emits it. `qucs/tests/test_net_highlight`
  covers the three joins, the selection union and the rendered glow.
- *Done:* **ERC pre-flight panel.** `qucs_s::erc::check(Schematic*)`
  (`qucs/erc.*`) walks the components and nodes: open pins (a node with
  the pin alone), loose wire ends (a node with one wire, no component and
  no label on it or its wire), a name used twice, and - for circuits
  without ports - no ground and no simulation block; errors first.
  `MessageDock` got a *Problems* tab (`showProblems()`, icons, count in
  the tab title, `locateRequested` on a click) and `QucsApp` the
  *Check Schematic* action (F10, `Sim.Check`), `checkSchematic()` before
  every non-tuner run (the tab comes up on errors), and
  `slotLocateProblem()` (the pane and tab of the document, the component
  at the issue's place selected, `Schematic::centerOn()`). A survey over
  the 217 shipped examples (`QUCS_ERC_SURVEY=1 test_erc`) finds no
  errors; its warnings are real open pins and ends. `qucs/tests/test_erc`
  covers every check, a clean example, the action, the tab, the click and
  the pre-flight. `Issue::file` names the schematic of a finding;
  `QucsApp::slotCheckHierarchy()` walks `erc::subcircuitFiles()` (Sub
  components) breadth-first with a visited set, using open documents and
  loading the rest off-screen, and `slotLocateProblem()` opens another
  file's issue with `gotoPage()`. The *Hierarchy and Netlist* toolbar
  (right of the simulation toolbar) carries `intoH`, `popH`, *Check
  Schematic* (the schematic in front only; a yellow check mark,
  `bitmaps/svg/check_current.svg`, the green one's colours turned
  yellow), this check, *Generate Netlist* and *Save netlist* (two new
  icons, `bitmaps/svg/netlist_*.svg`).
- *Done:* **The ground a setting.** ngspice, SPICE OPUS and Xyce
  (`AbstractSpiceKernel::checkGround()`) refused a circuit without a
  ground symbol, and the check called it an error, whatever brought node
  0 in - a net labelled `0`, a SPICE netlist or library part.
  `QucsSettings.RequireGround` (`RequireGround`, default on), a checkbox
  under *Simulators Settings → Before a simulation*: off, the kernels
  let the netlist go and the check reports a missing ground as a warning.
  `test_erc` covers both kernels and the check either way, the setting
  kept across a save and a load, and the dialog.
- *Done:* **Every line of a `.OPTIONS` section, however it is written.**
  `ComponentDialog::writeEquation()` read one `name = value` per line and
  threw away anything else, so an ngspice option that is a flag
  (`noopiter`, `keepopinfo`, `notrnoise`, ...) vanished without a word
  and `gmin=1e-10 reltol=1e-4` became one option whose value was
  `1e-10 reltol=1e-4`. `ComponentDialog::readOptionLine()` reads a line
  of the `.OPTIONS` editor into the options it holds - any number of
  them, with or without a value, after an optional `.option`/`.options`
  keyword, comments passed over - and `SpiceOptions::getExpression()`
  writes an option without a value on its own. The other equation
  components are untouched. `qucs/tests/test_spice_options` covers each
  shape, the netlist, what the editor shows next time and the file.
- *Done:* **A toolkit for a subcircuit's symbol.** `.PortSym` carries a
  name that `Component::analyseLine()` used to drop; it now reaches
  `Port::Name`, and `Subcircuit::readPortDirections()` reads the type of
  each port component out of the subcircuit's own file into `Port::Dir`,
  so an instance knows what its pins are called and which way they
  point without any change to the file format. `Component::drawPins()`
  writes the name just inside the end of the pin's stub - found from the
  line that starts or ends at the port, so it fits whatever the symbol
  is drawn like - and, under the second setting, a mark for the
  direction; both follow the component's rotation and mirroring because
  they are worked out from the transformed ports. `ShowPinNames` (on)
  and `ShowPinDirections` (off) are settings.
  `Schematic::buildDefaultSymbol()` replaces the fixed 40-wide box: it
  sizes the box to the pin names (`misc::pinFont()`) and, with the
  directions shown, puts the inputs on one side and the outputs on the
  other; `recreateSubcircuitSymbol()` drives it again over an existing
  symbol, which nothing could do before. `saveSymbolToFile()` and
  `loadSymbolFromFile()` move a symbol between documents, matching the
  incoming ports to this schematic's by number
  (`PortSymbol::placeLike()`). `PinOrderDialog` (`dialogs/`) rewrites
  the `Num` of the port components and the `numberStr` of the symbol's
  pins together. `PolylinePainting` (`paintings/`) is a painting for
  `qucs::Polyline`, the drawing primitive components already had:
  clicking the last corner ends the run, the first one closes it, and
  `Component::analyseLine()`/`SymbolWidget::analyseLine()` read it out
  of a `<Symbol>` block, so it reaches the instances like a line.
  Found on the way: `ImageWriter::noGuiPrint()` painted into a QImage it
  never filled (the interactive export does), and `fillComponentsList()`
  dereferenced the icon a painting is registered without.
  `qucs/tests/test_symbol_tools` covers all six.
- *Done:* **One name for a subcircuit's pin, its net and its symbol.**
  `AbstractSpiceKernel::createSubNetlist()` writes the `.SUBCKT` header
  from `pc->Ports.first()->Connection->Name` — the net the port sits on
  — while `Schematic::adjustPortNumbers()` wrote the port component's
  refdes beside the pin, so the symbol and the netlist disagreed and an
  unlabelled net reached the netlist as `_net7`. `Schematic::netLabelOf()`
  walks the net from a node and returns its label;
  `Schematic::portPinName()` is that label or, failing it, the port's
  name, and the symbol pin takes it. `Schematic::nameUnlabelledPortNets()`
  runs in `giveNodeNames()` between the two `throughAllNodes()` passes —
  after the labelled nets have propagated, so a port node still unnamed
  is on a net without a label anywhere — and gives that net the port's
  name, skipping a name another net already answers to (which would join
  two unconnected nets) and a name that is not a plain identifier. The
  ERC warns about the skipped case. Over the shipped examples all 22
  subcircuit definitions come out with named pins and no duplicated node.
  `qucs/tests/test_symbol_pins` covers the label, the fallback, a label
  elsewhere on the net, a renamed port, the netlist agreeing with the
  symbol in each case, and the collision.
- *Done:* **Images travel with the document, and reach the symbol.**
  `qucs_s::EmbeddedImage` (`qucs/embeddedimage.*`) holds the bytes of the
  file an image came from, its format, and the quarter turns and flip put
  on it; it renders at the size it is asked for, so an SVG is drawn from
  its source at every zoom instead of being frozen into pixels, and a
  photograph keeps its own compression instead of being re-encoded as
  PNG. `ImagePainting` is built on it: the file dialog offers every
  format `QImageReader` has (SVG and SVGZ added, as Qucs-S renders them
  itself through Qt6::Svg), `save()` writes
  `ImagePainting x1 y1 x2 y2 <base64> <format> <turns> <mirrored>` (the
  three new fields default, so 26.1.2 documents load and 26.1.2 loads
  ours), and `rotate()`/`mirrorX()`/`mirrorY()` turn the picture and not
  just its frame. A `qucs::Image` drawing primitive carries the same
  object into `Component`, where `analyseLine()` reads an `ImagePainting`
  line of a `<Symbol>` block - it was dropped before, so an instantiated
  subcircuit lost the picture - and `drawSymbol()`, `rotate()`,
  `mirrorX()` and `mirrorY()` treat it like the lines and arcs around it;
  `SymbolWidget` does the same for the library preview.
  `qucs/tests/test_symbol_image` covers the formats, the round trip with
  the source file deleted, the turns and mirrors of both the painting and
  the component, the older five-field line, and an image that cannot be
  decoded (which no longer refuses the whole document).
- *Done:* **Unit-aware property editor**. Upstream's redesigned
  component dialog (#1054) had already replaced the two-column table and
  hides the properties the simulator in use does not take; what was left
  is the units. `qucs_s::units` (`qucs/valuereading.*`) reads a value:
  a number (a prefix and a unit after it as Qucs writes them - or in
  another case, which is then compared), an expression, a name, a list,
  or text (a model name like `2N2222`, which merely starts with a
  digit); for a number, what Qucs reads (`misc::str2num`), what the
  SPICE netlist carries (`spicecompat::normalize_value`, the
  netlister's own) and what SPICE reads in that (`spiceNumber()`: the
  number, a scale factor with M = milli and MEG, the rest ignored), with
  a warning when the two differ (`10 Mohm`, `10 meg`, `1 KOhm`) or a
  digit follows the prefix (`4k7`, `2R2`). `ComponentDialog` shows it in
  a line under the property table for the value field in focus, as it
  is typed, colours a field with a warning amber, and puts the
  description on the name and value cells as a tooltip. Over the
  defaults of every built-in component (1086 numbers) nothing is
  flagged. `qucs/tests/test_value_reading` covers 22 value shapes, the
  warnings, SPICE's reading, the notation and the dialog.
- *Done:* **Subcircuit properties dialog redesign** (#1285). `ID_Dialog`
  (`paintings/id_dialog.*`) kept its name and the file format
  (`"1=name=default=description=type"` in the `.ID` line) and lost the
  rest: a read-only table, four edit fields, a check box and an Apply
  that copied the fields into the selected row. Now one `QTableWidget`
  (Show as a check box; Name, Default, Type, Description editable in
  place) with an item delegate whose editors take only what the field
  may hold (the old validators, and a type combo of real / integer /
  string that can be typed into); Add (a free `P<n>` name, the editor
  open), Remove (every selected row), Move Up/Down (the whole row);
  `problem()` checks the prefix and every row - a name missing, twice,
  "File", or a character the format cannot hold, also for text that
  did not come through an editor - and `apply()` shows it at its cell
  or writes the table into the `ID_Text`, repainting the schematic.
  Apply keeps the dialog open, and `ID_Text::Dialog()` reports a change
  also when Apply wrote one before a Cancel. New-style connects, no
  hand-deleted members. `qucs/tests/test_subcircuit_dialog` covers the
  table, edits reaching the symbol and reading back, OK without a
  change, adding, removing and moving rows, every refusal, Apply (also
  through `ID_Text::Dialog()`), and the editors.
- *Done:* **Project-wide search/replace of component values** and a
  "find component by refdes" box on the canvas. `qucs_s::search`
  (`qucs/componentsearch.*`) is the logic, widget-free: a `Query` (text;
  names and labels and/or values; case, whole value, regular
  expression; a model, a name wildcard, a property) and `find()`, which
  lists `Match`es - exact names first, then other names, net labels
  (of wires and nodes), values, each in document order; `replaced()`
  (whole value when there is no text, `\1` groups for a regular
  expression), `replace()` (the component found again by name and
  place, the value only if it is still what was found and the new one
  is neither empty nor holds a `"`; the text keeps its distance to the
  symbol as the properties dialog does; the caller records one undo
  step) and `reveal()` (selects the component, or a label and a wire of
  its net so that the net highlighting shows it, and centres it).
  `FindBar` (`qucs/findbar.*`) is a row under each pane's tabs
  (`PaneWidget`), opened by *Edit → Find* in a schematic - the action
  was disabled for schematics, and its slot C-cast any document to
  `TextDoc` - searching names, labels and values as typed, one stop per
  component, and following the pane to another document.
  `FindReplaceDialog` (`dialogs/findreplacedialog.*`, non-modal, one per
  application) replaces `ChangeDialog` behind *Edit → Replace* (F7) for
  schematics: scopes *This schematic*, *Open schematics* (unsaved
  changes included) and *All schematics of the project* (`.sch` below
  the project, `Scratch/` excepted, the closed ones read off-screen),
  type and property lists taken from the open schematics, a checkable
  preview with the new values, Replace disabled once the fields no
  longer describe the search shown. A closed schematic with checked
  hits is opened with `gotoPage()`, changed and left unsaved (an
  off-screen load and save would also reset the file's view).
  `QucsApp::showDocument()` (pane and tab to the front) is shared with
  the Problems tab. `qucs/tests/test_component_search` covers the
  search, the replacement rules, stale and refused values, undo, the
  bar (typing, stepping, wrapping, labels, another document) and the
  dialog (checked rows only, the old dialog's "set this property"
  use, stale fields, a bad expression, a project with an open, a closed
  and a Scratch schematic, the double click).
- *Done:* **Export in every useful format, by Qucs-S alone.** Upstream's
  `ImageWriter` wrote PNG and JPEG through `QImage` and SVG through
  `QSvgGenerator`, and handed PDF, EPS and PDF + LaTeX to Inkscape with
  0.92's options (`-z --file= --export-pdf=`), which Inkscape 1.x rejects
  or warns about; without Inkscape: "Inkscape start error!". The SVG had
  no `viewBox`, a size in millimetres at 72 dpi read at 96, and the
  drawing fitted into the size of the schematic without its margins:
  0.92 of the canvas, in its top left corner. Its text named the font as
  Qt knows it - on macOS `.AppleSystemUIFont`, which nothing else can
  resolve - so every reader set it in another font and the labels ran
  over their boxes. The command line refused JPEG and printed a PDF on
  an A4 page. `qucs_s::graphicsexport` (`qucs/graphicsexport.*`) now
  writes PNG, JPEG, BMP, TIFF, WebP (those Qt's image writers have), SVG,
  PDF (`QPdfWriter`, the page the size of the drawing), EPS and PDF +
  LaTeX; `Schematic::printedArea()` is what `print()` draws (the frame
  included), so the size and the drawing agree. Two paint devices
  (`qucs/exportdevices.*`): `RelayDevice` draws on another painter and
  decides the text - as it is (a font of a family starting with "." is
  named `sans-serif` in an SVG), as the outlines of its glyphs, or kept
  with its position, angle, font and colour for a LaTeX overlay - and
  the colours: grayscale, or black and white, where a line or a text is
  black unless it is white and a fill white unless it is dark, so light
  fills do not swallow the drawing and cyan traces do not vanish.
  `EpsDevice` writes EPS (LanguageLevel 2, a unit 1/96 inch, y turned
  down in the prolog): paths with their fill rule, pens with width, caps,
  joins, miter limit and dashes (a cosmetic pen one unit wide), the
  hatch patterns as clipped lines, the dense ones as their share of the
  colour, alpha and opacity on white paper, clipping, images as
  hexadecimal samples, text as outlines. PDF + LaTeX writes `NAME.pdf`
  without text and `NAME.pdf_tex` in Inkscape's layout (`\svgwidth`,
  `\svgscale`), each text at the start of its baseline, turned, in its
  colour, bold or italic, as large as in the drawing unless
  `\qucsdocumentfont` is defined, with what LaTeX takes for a command
  escaped unless the text has a `$`. The dialog (rewritten) has the
  format, the scale linked to the resolution and the pixels, the size in
  centimetres for a vector format, colours, transparency (not for JPEG
  or BMP), quality (JPEG, WebP), text as outlines (SVG, PDF), a note for
  EPS and PDF + LaTeX, and *Copy to Clipboard*; the choices are kept
  (`Export/` in the settings) and the file offered is named after the
  document. *Edit → Copy as Image* (and the context menu) puts the
  selection or the document on the clipboard: an image at twice the
  scale, an SVG with outlines and a PDF. The context menu of the empty
  canvas ends with *Export...* (the dialog; not on a component's); a diagram's *Export Diagram...* (the
  diagram alone) is one action instead of a new one per right click. `-p` takes the same
  formats by the extension, `--dpi` for an image, `--color BW`; a PDF is
  the drawing's size unless `--page` or `--orin` is given, and an
  unknown extension or an empty document is an error (exit 1) instead
  of nothing written. `qucs/tests/test_graphics_export` covers the
  formats and suffixes, the area and the selection, every raster format,
  scale and resolution, paper and transparency, grey and black and
  white, the SVG's size and geometry (rendered and compared with the
  image), its fonts, the PDF's page and fonts, the EPS's structure and
  the EPS engine alone, PDF + LaTeX, the text kept for LaTeX, outlines
  against text, the clipboard and the dialog; where Ghostscript,
  pdftoppm and pdflatex are installed it renders the EPS and the PDF and
  compiles the overlay. The `load` smoke suite writes an example in every
  format from the command line.

**Larger (architectural — see WS4 first)**

- **Migrate the canvas from `Q3ScrollView` to `QGraphicsView`.** This is the
  single biggest UX unlock (hover, per-item caching, smooth zoom, GPU,
  selection rubber-band for free, item-level dirty rects) *and* removes the
  vendored Qt3 compatibility layer. It is also the riskiest change in the
  codebase. Recommend doing it only after WS1.3/1.5 and a test harness exist,
  and doing it as a new `Schematic` view class behind the same `QucsDoc`
  interface so both can coexist during migration.
- **Command-pattern undo** (replace whole-document snapshot strings with
  `QUndoStack` commands). Enables selective undo, undo across symbol/schematic
  mode, and lower memory; also eliminates the leak in 1.3 by construction.
- **Component definitions as data** (upstream #1047, "migrate C++ hardcoded
  devices to XML"): symbols and property lists in JSON/XML loaded at runtime.
  Makes the palette user-extensible without a compiler and shrinks
  `components/` by ~35k lines.

### WS4 — Architecture modernisation (enabling, incremental)

Do these opportunistically, file by file, as WS1–WS3 touch the code:

1. `std::unique_ptr` ownership in the five document lists; non-owning
   observers become `QPointer`/raw. Start with `Diagram`/`Graph` (smallest
   blast radius, highest crash yield).
2. Extract a `Dataset` model class from `Graph::loadDatFile` — parse once,
   share across diagrams, expose typed accessors. Diagrams then never touch
   `char*`.
3. Replace `QucsMain->` and `QucsSettings.` global reach-ins with injected
   references, starting at `AbstractSpiceKernel` (already takes a
   `Schematic*` — extend to a settings interface). Makes the kernels
   testable headless.
4. Finish the settings migration (`tQucsSettings` struct → `settingsManager`)
   so there is one source of truth.
5. Split `QucsApp` (4.2k lines) along the seams the header already documents:
   `DocumentManager`, `ProjectBrowser`, `SimulationController`.

---

## 3. Suggested first milestone (≈2–3 weeks)

1. Set up CI: Debug+ASan and Release builds on macOS + Linux (WS2.1, 2.2).
2. Reproduce #1711 locally with the attached zip under ASan — expect it to
   point at `Graph::loadDatFile`. Fix per WS1.1/1.2, add the dataset to a
   regression test (WS2.3).
3. Land WS1.3 (`documentRebuilt` + free-on-rebuild), WS1.4, WS1.5. These are
   mechanical and self-contained.
4. Land WS1.7 (autosave + crash handler) so that any remaining crash costs
   nothing and yields a stack trace.
5. Ship a first "hardened 26.1.1" build for macOS from this repo's `bin/`.

After that, WS3 quick wins are safe to start; each one adds a feature *and*
retires a fragile code path.

## 4. How to verify the crash fixes

Reproduction recipes for the classes in §1, usable as regression tests:

- **Dataset**: take any `examples/ngspice/*/*.dat` produced by a simulation,
  truncate it mid-`<dep>` block, open the matching `.dpl` → must show an
  "invalid data" graph, not crash.
- **Undo lifetime**: open the tuner on a resistor, then Ctrl-Z until the
  resistor is gone, then move the tuner slider → must disable the row, not
  crash.
- **Tab-switch during simulation**: schematic with DC-bias enabled, press
  Simulate, immediately switch to a `.m` tab → completion must open the
  correct `.dpl`, not crash.
- **Cast audit**: open a text document as the current tab and invoke every
  schematic-only action from the menus → each must no-op or show a message.
- **Short branches**: `.dat` with a dependent variable of 1 and of 2 samples
  on a rectangular diagram in each graph style → must render.
