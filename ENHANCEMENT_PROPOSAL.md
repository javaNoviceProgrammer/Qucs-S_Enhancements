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
- *Done:* **Content panel lists the whole project tree.** Upstream only
  showed the files in the project directory itself. `misc::projectFiles()`
  walks the subdirectories (no hidden directories, no symlinked ones) and
  the tree shows `sub/dir/name.ext` under the category, root files first.
  Everything that took the row text as a file name relative to the project
  still does (open, delete, subcircuit insert); copy and rename keep the
  file in its subdirectory, and `misc::properAbsFileName()` resolves such a
  relative path against the project so an inserted `sub/x.sch` netlists
  from any schematic. New *Osdi* category for the compiled models; the
  ngspice netlister `pre_osdi`s every `.osdi` of the tree and Build All
  compiles every `.va` of the tree, so the panel and the simulator agree
  on what belongs to the project.
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
- **Explicit light/dark theme toggle** (#1725). The `hasDarkTheme` flag
  already exists in `QucsSettings`; components draw with hard-coded
  `Qt::darkBlue` pens, so this needs a small palette-indirection layer in
  `qucs::DrawingPrimitive::draw()`.
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
- *Done:* **Cancel keyboard move restores original position** (#1525).
  A cursor-key move now marks the document modified and is one undo step
  however many key presses it takes (`setChanged(..., 'k')` coalesces
  like marker moves); while it is the latest step, Escape takes it back
  (`Schematic::cancelKeyboardMove()`). A mouse press, another edit, undo
  or redo closes the sequence. Upstream neither recorded the move nor
  marked the document changed, so a keyboard-moved schematic could be
  closed without a save prompt. `qucs/tests/test_keyboard_move` covers it.
- **Open any file regardless of current simulator mode** (#1468): today the
  component palette is torn down and rebuilt on simulator switch
  (`Module::registerModules`), so a `.sch` for another backend fails to
  resolve components. Fix: register *all* components once and filter the
  palette view instead of the registry.
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

**Medium (weeks each)**

- **Auto-placement of DC-bias labels** to avoid overlaps (#1692).
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
  (right of the simulation toolbar) carries `intoH`, `popH`, this check,
  *Generate Netlist* and *Save netlist* (two new icons,
  `bitmaps/svg/netlist_*.svg`).
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
- **Unit-aware property editor**: a single inline editor that understands
  SI suffixes, validates against the property's `spicecompat::Simulator`
  mask, and shows the description tooltip — replacing the two-column
  name/value table.
- **Subcircuit properties dialog redesign** (#1285).
- **Project-wide search/replace of component values** and a
  "find component by refdes" box on the canvas.

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
