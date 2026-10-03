# Bug hunt: the PRBS source and eye diagram, the stress-report fixes, the Projects menu

*1 October 2026 - Qucs-S 26.1.5, `7d91ad7` (the ASan build at `6b2df9f`). One hour,
20:59 to 21:57.*

This hunt covers what changed on 1 October:
- the V(PRBS) source and the eye diagram (`d19221d`), and the eye's unit interval from
  the source's Tbit (`e7499b2`);
- the fixes of the stress report (`1b64aaa`): more than 999 nets, the no-results error,
  components freeing what they own, compressed undo steps, mapped datasets, ground refs
  and once-each messages, the kept selection of a preview, the transient-step note;
- Unlink Project, Open Project and Close Project in the Projects panel's menu (`6b2df9f`,
  `7d91ad7`).

It ran in the foreground on macOS arm64 with ngspice 46, an ngspice built with the PRBS
source. Each `qucs-s --mcp-server` had its own workspace, settings, HOME and trash in a
scratch folder. Methods:

1. **Probes through the tools**, numbered p1 to p41 below. The larger ones are files
   beside this report (`probe.py`, `chain.py`, `p*.py`); the others were run inline and are
   described in `notes.md`. They covered:
   - the PRBS source's values, simulated;
   - the eye analysis on made-up signals (flat, one edge, two points, NaN, time running
     backwards or repeated, 2 million points);
   - eye diagrams loaded from files with damaged fields;
   - the eye's dialog, with bad values;
   - more than 1000 nets with an equation, a sweep, AC, DC, FFT, and a transient with a
     DC point;
   - the step note on odd transients;
   - previews, refs and the undo marks;
   - the Projects menu on links, folders and dangling links.
2. **Every ngspice example simulated** whose file runs nothing besides the simulator (88
   of 114; the others name a command part, a shell, a script or a SPICE init block, and
   were left alone).
3. **An ASan/UBSan build** (Debug, as CI's `linux-asan`), used for:
   - the parts whose lists are rebuilt (MUTX, EDD, RFEDD, a subcircuit gaining a port, a
     library part) through edits, undo, redo, replace and reopening;
   - unlinking the open project;
   - a tool fuzzer (`p23_fuzz.py`) over select, move, delete, undo, redo, `undo_history`,
     `diff`, `edit_diagram`'s eye, `get_dataset`'s eye, `edit_component` of the PRBS
     source and `add_component`, previews included.
4. **The project's fuzzers and suites:**
   - `fuzz-sch.py`, seeds 1001 and 2002, on ASan, with this hunt's eye and PRBS
     schematics among the seeds;
   - `fuzz-simout.py`, seed 7, on ASan, over a corpus from `smoke-test.sh simulate`;
   - `smoke-test.sh hostile`;
   - `check-tool-arguments.py`;
   - the GUI monkey, five new seeds.
5. **CI's latest runs**, read once.

## Summary

| | Finding | Since |
|---|---|---|
| **A1** | A run with no results, in a folder that takes no new file, empties the dataset it says it kept | `1b64aaa` |
| **A2** | CI's tool-argument check fails: `get_dataset`'s dispatch no longer reads as one | `1b64aaa` |
| B1 | A linked project whose folder is gone is not listed: it cannot be unlinked, and its name stays taken | `6b2df9f` (the panel: before) |
| B2 | `get_state`'s `selected` names a selected ground `""` | before |
| B3 | The eye folds old data at a Tbit changed since the run, and says "V1's Tbit" | `e7499b2` |
| B4 | An eye drawn as traces over 100,000 bits: 11 to 18 s per repaint, at every zoom | `d19221d` |
| B5 | Open Project can be chosen on the open project when its path is spelled another way | `7d91ad7` |
| B6 | A selection holding two parts of one name: move and delete act on the first only | before |
| B7 | A PAM4 source's trace is measured as NRZ unless told: its Coding is not read | `e7499b2` |
| B8 | One eye diagram over two PRBS sources folds both at the first one's Tbit | `e7499b2` |
| C1 | CI red on every run of the day: two macOS unit tests | before |
| D1-D4 | Examples and analyses: a DC sweep of a parameter, a missing library, an example that does not open, one that does not converge | before |
| N1-N5, notes | Minor: the PRBS values unchecked, the step note's edges and wording, the eye dialog, the grounds in findings, messages | various |

No crash, hang or sanitizer report anywhere.

## A. Regressions of the day

### A1. The dataset "from before" is emptied when its folder takes no new file

`convertToQucsData` writes the dataset through a `QSaveFile` with
`setDirectWriteFallback(true)`. In a folder that takes no new file, but where the dataset
can be written (folder 555, dataset 644), the fallback writes into the dataset itself. When
the run then has no results, `cancelWriting()` leaves the file truncated to the header
already written: 22 bytes, `<Qucs Dataset 26.1.5>`. The error says the opposite:

> The simulator wrote no results: its output (spice4qucs.tr1.plot) holds none. Look for
> an error in its log - the dataset is the one from before.

Reproduced (`p9`) with a stand-in simulator that prints a line and exits 0, the
old dataset 95 bytes of a real transient, after the run 22 bytes. With a folder that takes
new files the old dataset stays, as tested.

*Fix:* without a new file, write nothing at all when no results are expected to come:
decide "no results" before opening the dataset (the outputs are on disk then), or open the
dataset in place only once the first variable is ready to be written. Turning the direct-write
fallback off is another way: the folder already gets the "check write permission" error.

**Fixed in `e1723e1`.** The dataset is opened only when the run's results are whole: in a folder that takes no new file (the dataset itself writable), they are written into a temporary file and copied over the dataset then, so a run with no results leaves it as it was. Test: `test_scale_and_memory` `anEmptyOutputIsAnError` (a folder of mode 555: no results, then results, then no dataset yet); writing in place again is caught.

### A2. CI's "Claude's tools take what they read" fails

`scripts/ci/check-tool-arguments.py` reports `get_dataset: no handler found in call()`:
the dispatch became a block (`getDataset(args)`, then `misc::releaseFreedMemory()`), and
the checker looks for `return getDataset(args);`. CI's Linux job stops at this step on
`6b2df9f`. It passed this step on `d19221d`.

*Fix:* release the memory inside `getDataset` (or a wrapper the checker knows), keeping
the dispatch a single `return`.

**Fixed in `e1723e1`.** `getDataset` offers the freed memory back on its way out (a scope guard), and the dispatch is one `return` again. The check runs in ctest too (`check_tool_arguments`), so the local suite catches what only CI did.

## B. Bugs

### B1. A linked project whose folder is gone

A link in the workspace whose project folder was moved, deleted or is on a drive not
mounted is not listed in the Projects panel (`QFileSystemModel` leaves broken links out).
So it cannot be unlinked or deleted there; `context_menu` says "gone_prj is not in the
Projects panel". Yet Import and Link Project still find the name taken:
`workspace::check` counts `isLink(path)`, giving "The workspace has a gone_prj already."
for a folder the user cannot see (`p6`).

*Fix:* list a broken link (in grey, its target as the tooltip, "missing"), so that Unlink
reaches it; or have Link Project offer to replace a broken link of that name.

**Fixed in `e1723e1`.** The panel lists a link that leads nowhere: the model reads with `QDir::System` (a broken link is one) and the filter leaves the system's other files out. It is greyed, its tooltip where it led and that Unlink Project removes it; a double-click says so rather than listing nothing. Import and Link Project offer to replace such a link of the name, and `workspace::check` says why the name is taken. Test: `test_workspace_projects` `aLinkThatLeadsNowhere` (a FIFO beside it stays unlisted).

### B2. `get_state` names a selected ground `""`

`qucscontrol.cpp:4482` lists the selected parts by `c->Name`: a selected ground is `""`,
not its ref `GND#2` that `select`, `move` and `delete` take. This is the naming of the
stress report's bug 2, in a place its fix did not reach (`p13`, `p34`).

*Fix:* `PartIndex::refs(sch)` there too.

**Fixed in `e1723e1`.** `get_state` names each selected part by its ref. Test: `test_qucs_control` `twoPartsOfOneNameAreToldApart`.

### B3. The eye folds old data at a Tbit changed since the run

The eye's unit interval is the PRBS source's Tbit *now*, while its data may be from a run
before the Tbit changed (`p17`):
- simulated at 100 ps, then Tbit 200 ps without a run: the diagram keeps its cached
  100 ps until the data is read again;
- then (`reload_data`, reopening) it folds the 100 ps data at 200 ps: height 2e-13,
  jitter 100 ps, still "unit interval from: V1's Tbit". `get_dataset`'s eye does the same.

Deleting V1 likewise leaves "V1's Tbit" shown until a reload, then "the crossings".

*Fix:* the run's netlist is kept beside its dataset (`misc::keepRunNetlist`): take the
Tbit from the `PRBS(...)` line of that netlist, or fall back to the crossings (and say
why) when the source's line differs from the run's.

**Fixed in `e1723e1`.** The source's Tbit and Coding are those of its line in the run's netlist (`misc::runNetlistOf`, kept for the dataset as it is); the source's values now count only without one. A Tbit or Coding changed since is said - "V1's Tbit is 200 ps now, 100 ps in the run the data is of: simulate again to see it" - in the diagram and in `get_dataset`; a source not in that run is none, and why. A deleted source's cached "V1's Tbit" is now true of the data until the next reload. Tests: `test_prbs_eye` `theSourceIsAsTheRunGaveIt`, `eachTraceIsFoldedAtItsOwnSource`; `test_qucs_control` `resultsAreMeasuredAsTablesSpectraAndEyes`.

**Found after the fix (3 October).** B3's fix read the run's line the Qucs way. The netlister writes 100 ps as `100P`, and Qucs's reading knows no uppercase P: 100 s. Each eye of a run kept so said "V1's Tbit is 100 ps now, 100 s in the run the data is of: simulate again", and the automatic unit interval was 100 s. B3's test wrote the line by hand in lower case (`100p`), which both readings take for pico. **Fixed in `ec06017`.** The line is read as ngspice reads it (`units::spiceNumber`: SPICE's scale factors of any case, M milli), and the source's own Tbit as the netlist gives it to ngspice (its `spiceValue`), so a Tbit written `100P` is 100 ps too. The test (`theRunsNumbersAreReadAsSpiceReadsThem`) keeps the netlist the netlister writes.

### B4. An eye drawn as traces: seconds to a minute per repaint

Over 100,000 bits (2 million points), `drawn: traces` takes the following per repaint
(screenshots, `p21`, `p37`):

| | Paint |
|---|---|
| span 2, first paint | 10.9 s |
| span 2, second paint | 15.0 s |
| span 8 | 58.8 s |
| after a zoom in | 18.4 s |
| after a zoom out | 14.9 s |
| cached, size unchanged | 0.02 s |

The window is frozen as long. Density draws the same in 0.4 to 0.9 s: it counts pixels;
traces draws every window as an antialiased, translucent polyline.

*Fix:* draw traces as density does - accumulate into a coverage buffer, without
antialiasing above a few thousand windows - or cap the windows drawn and say so.

**Fixed in `e1723e1`.** Up to 2,000 windows the traces are drawn as lines, as before; more, each pixel is as opaque as that many lines of the traces' alpha laid over it (1 - (1 - a)^n, the passes counted as the density counts them). Test: `test_prbs_eye` `manyBitsAreDrawnAsTracesQuickly`, 100,000 bits: no slower than three densities and a second (drawn as lines, 4.1 s against the density's 0.22 s on the test's diagram).

### B5. Open Project chosen on the open project

`slotProjectsContextMenu` compares the row's path with the open project's by
`QDir::cleanPath`, not by the folder. With the workspace given as `/tmp/...` (a link to
`/private/tmp`) and the project opened by its real path, the open project's own row offers
Open Project (`p25`). Choosing it closes every document (asking about unsaved changes)
and opens the same project again. The same holds for a workspace path through any link,
and for case on macOS.

*Fix:* compare canonical paths (`QFileInfo::canonicalFilePath`), as `openProject` does
for the workspace and home folders.

**Fixed in `e1723e1`.** The open project's row is told by the folder itself (`misc::isSameFile`), not its spelling. Unlink Project had the same fault: the open project and the documents through the link are found by the link's place with the folder it is in resolved (`placeOf`), so a workspace reached through a link, /tmp or another case finds them; a document of the project's own folder, not through the link, stays open. Test: `test_workspace_projects` `theSameFolderSpelledAnotherWay`.

### B6. Two parts of one name in a selection

A hand-edited file with two parts named R1 (Check Schematic says "R1: the name is used
twice"): Select All, then `move {selection: true}` moves R1 (1k) and the ground, and
leaves the other R1 (2k) where it was; `delete {selection: true}` deletes the first R1
only. Both answers list "R1" as done (`p33`). `withSelection` turns the selection into
names, and both resolve to the first. This was so before the stress fix, which kept it.

*Fix:* pass the selected parts themselves (or their index in `a_DocComps`) from
`withSelection` to the handlers, not their names.

**Fixed in `e1723e1`.** A name given twice is told apart by a ref as grounds are: the first R1, the second R1#2 - in `get_schematic`, a selection, `select`, `move`, `delete`, `edit_component` and pins (R1#2.1); R1#3 of two is refused with the range. The refs are made in one place (`erc::refs`), which Check Schematic and the tools share. Test: `twoPartsOfOneNameAreToldApart`.

### B7. A PAM4 source's trace measured as NRZ

The eye takes the unit interval from the V(PRBS) source ("unit interval from: V1's
Tbit"), but not the levels from its Coding. The eye diagram (levels 2 by default) and
`get_dataset` without `levels` measure a PAM4 PRBS7 through an RC as one NRZ eye
(levels 0.16 and 0.83, Q 2.03), saying nothing of PAM4 (`p35`). With `levels: 4` the
same data gives its four levels and three eyes.

*Fix:* with no levels given, take 4 from a source coded PAM4 (and say so, as for the
Tbit).

**Fixed in `e1723e1`.** The eye diagram's Levels default is "as the PRBS source is coded": a trace of a PAM4 source is measured on its four levels ("PAM4, V3's Coding" beside it); 2 or 4 set stay so, and 2 on a PAM4 source is said. `get_dataset` without `levels` does the same ("levels from"; data without four levels gets an error that says why four were tried). A diagram saved with 2 keeps it; `edit_diagram`'s `levels: null` is "as coded". Tests: `eachTraceIsFoldedAtItsOwnSource`, `resultsAreMeasuredAsTablesSpectraAndEyes`, `theSettingsAreSavedAndLoaded`, `theDialogEditsIt`.

### B8. One eye diagram over two PRBS sources

Traces from V1 (Tbit 100 ps) and V2 (200 ps) in one eye diagram are both folded at 100
ps. Trace 2's measurement says "unit interval from: V1's Tbit" (`p39`). `get_dataset` on
the same two variables gives each its own source's Tbit, so the two disagree.

*Fix:* each trace its own unit interval (the diagram draws one grid in UI: fold each
trace at its own), or say that trace 2's source has another Tbit.

**Fixed in `e1723e1`.** Each trace is folded at its own source's Tbit; a trace with none at the first source's, or at what the crossings tell, as before. When the traces' UIs differ, the time across it is in UI ("time in UI, 2 UI - each trace at its own"), the marks and the cursor readout with it. Test: `eachTraceIsFoldedAtItsOwnSource` (the PAM4 eye open in the frame's middle, closed when folded at V1's bits).

## C. CI

### C1. Every CI run of the day failed

Not from this round's code, but standing. CI read once, on `6b2df9f`:
- macOS's unit tests: `test_long_file_names` timed out (300 s);
- `test_qucs_control`'s `pinsAreGivenTurnedAndConnectSaysWhy` failed with "connect:
  via[0] is a list, not the number 600".

The test passes here. It builds `via` as `QJsonArray{QJsonArray{600, 300}}`: a
one-element brace list of the same type may be taken as a copy rather than a nested array,
depending on the compiler, so on CI's toolchain `via` is likely `[600, 300]`. The Linux
job failed its unit tests on `d19221d` too. The runs failed back to `14687a6` at least.

**Fixed in `e1723e1`.** Three causes, each reproduced or read from CI's log:
- `via` is built by append. A new check, `scripts/ci/check-json-nesting.py` (a CI step and a ctest), refuses a list of one list written as a braced copy anywhere in the sources.
- `test_long_file_names` waited on a box. CI's runner has no ngspice; an import of settings puts what its file lacks back to the default (ngspice's path too) and refilled the simulators, which said "No simulation backend found" in a modal box - over the import, then at each window's start. Reproduced here with ngspice off the PATH. The box is said once until a simulator is found again, never over an import (its report says it), and the test puts its simulator back after the import.
- `test_workspace_projects` timed out too (missed in the hunt's reading): it waited in `unlinkingAProject`, a menu opened and nothing after. Not reproduced here; the likely cause is its menu helper pressing Return on an entry that could not be chosen there (the row not found for a moment), which leaves a menu open. It closes such a menu now and says so, and the test waits for the row first.

Every test that isolates its settings now has a watchdog (`tests/isolated_settings.h`): a dialog or a menu open over a minute is logged with its words and closed, so a hang fails with its reason, not at ctest's timeout. Both suites pass with ngspice off the PATH.

**After the push (2 October).** CI's run on `d4ff98e`: macOS's unit tests and the Windows build pass, both timeouts gone. The Linux job (GCC, ASan and UBSan, Ubuntu 24.04's ngspice 42) still failed, for two causes the hunt had not read:
- `test_scale_and_memory.cpp` included `<sanitizer/allocator_interface.h>`, which only clang installs: since `1b64aaa` the job stopped at the build.
- Before that, `test_qucs_control`'s `aModuleNotLoadedIsExplained`. ngspice 42 says "could not find a valid modelname", without the model's name, and the hint for a Verilog-A module not loaded waited for "Unable to find definition of model X", as the ngspice here (46) says it. Users of that ngspice got no hint.

**Fixed in `85e9b7e`.** The function is declared where the header is missing (GCC's libasan has it); the model is the word of the device's netlist line that has a `.model` card, and the test runs a stand-in that answers as ngspice 42 does, with an ngspice here or not. Not run on Linux here (no container or GCC on this machine): CI's next run tells.

## D. Examples and analyses (before the day)

- **D1. A DC sweep of a parameter aborts, with no warning.** A `.SW` over `.DC` of a
  `SpicePar` parameter is netlisted `dc rv 1000 3000 1000`. ngspice's `dc` sweeps
  sources, resistors and the temperature, so it gives "dc simulation(s) aborted" and
  "Error during 'write': no writable vector found.", at any size. Sweeping R1 or V1 works.
  Check Schematic says nothing (`p5`).
- **D2.** `Devices/MESFETs/testACMESFETCL1.sch`, `testDCIdsVgs.sch` and
  `testDCMESFETCL1.sch` take `MESFETCL1` from a library `MESFETs` that is not shipped.
- **D3.** `OpenVAF/Tunnel_Ngspice_prj/tunn.sch` cannot be opened, not even with its
  project open: its part `tunnel` is a Verilog-A module not built yet. `open_document`
  says only "could not be opened."
- **D4.** `General Electronics/Waveform Generation/sawtooth-2.sch`: "Timestep too small"
  with this ngspice 46, before the day too.

**Fixed in `e1723e1`.**
- D1: Check Schematic gives an error for a `.SW` over a DC analysis whose Param is no source, resistor or temperature (ngspice, SPICE OPUS), naming what does it: an NgSweep with Analysis op. Test: `test_erc`.
- D2: `library/MESFETs.lib` again: MESFETCL1, the Curtice level 1 equations of the earlier library's XSPICE code model (Mike Brinson's `curtice1`, from an older Qucs-S tree, GPL) as ngspice B sources - this Qucs-S has no code-model compiler. Two changes, in its description: the gate current continuous at vbi (it jumped to vbi/rf there), and 1 kOhm across each lead inductance, damping a resonance at GHz that the AC example's transient rang on. The three examples simulate; the drain current matches the equation to seven digits; the library job's results list it as passing.
- D3: `open_document` says why: "could not be opened: Unknown component: tunnel. tunnel is the Verilog-A module of .../tunnel.va, not built yet: build it (build_verilog_a with that file), then open this again"; the window's question says the same. Test: `test_qucs_control` `whyADocumentCouldNotBeOpened`.
- D4: the example's `.TR` asks for Gear, which ngspice never got: the integration method is Qucsator's. The example has a `.OPTIONS` part with method=gear now (Gear runs it; the trapezoidal rule, and Gear of order 1, stop), and Check Schematic notes a `.TR` method other than the trapezoidal rule under ngspice (three other examples get the note).

## N. Minor

- **N1.** The PRBS source's values are not checked before a run: Order 1, 40 or "seven",
  Tbit 0 or negative, Tr longer than Tbit, Seed 0. ngspice refuses each, clearly, at the
  run. An unknown Coding ("pam8") is netlisted as NRZ without a word.
- **N2.** The step note takes a pulse's edge as the faster of rise and fall. With Tr 0
  and Tf 1 ns, the 0 counts as "the step itself": no note, though the 1 ns fall is
  crossed in a tenth of a step. A negative MaxStep or Tr is not flagged (netlisted as
  given), and Points "2001.5" reads "in 2001.5 points".
- **N3.** With MaxStep set, the step note ends "A MaxStep of X or less (or more Points)
  resolves them". ngspice's step limit is then MaxStep alone, so more Points do not.
- **N4.** The eye diagram's dialog takes bad values without a word and drops them:
  - Unit interval -1 or 1e400, From "abc", Threshold "nan";
  - Mask width 5 or 0, Mask height -1 or 1e400.

  A valid mask set before is removed, and the unit interval goes back to automatic.
  `edit_diagram` refuses the same values.
- **N5.** Check Schematic's findings about a ground give `at`, not the ref (`GND#1`)
  that select, move and delete take.
- Messages and small costs:
  - `open_document` of a file with an unknown part says "could not be opened." with no
    reason;
  - `select`'s "Not found: There are 3 of GND ... GND#3.." ends in two periods;
  - `diff {steps: 1}` unpacks every kept undo step (0.4 s at 10,000 parts);
  - after Unlink Project the panel lists the removed link for one to three seconds, its
    entries off.

**Fixed in `e1723e1`.**
- N1: Check Schematic gives an error for each V(PRBS) value ngspice refuses, checked against ngspice 46: a Tbit of 0 or less, a Td below 0, an Order not a whole number from 2 to 31, a Seed not a whole number above 0 or with its low Order bits all zero, a Tr or Tf longer than the Tbit; a warning for a Tr or Tf below 0 (taken as 0: the same waveform) and a Coding neither NRZ nor PAM4.
- N2: the step note takes a source's shortest edge that takes time (a rise of 0 hid a fall of 1 ns; a PWL's shortest, not its steepest). A MaxStep below 0 is an error ("TMAX is invalid"), Points not a whole number a warning, a pulse's edge below 0 a warning.
- N3: with MaxStep set, the note offers only a shorter MaxStep.
- N4: the eye diagram's dialog refuses a value it cannot take, says why, and puts the focus in its field; nothing is applied, the mask set before stays; a mask needs both values.
- N5: a finding about a part its name does not tell carries its `ref` (GND#2, R1#2), and a click on it in the Problems tab selects that part.
- Messages: `open_document`'s reason (D3); one period after "Not found: ..."; `diff` by steps and `undo_history` unpack only the states they compare; after Unlink Project or Delete Project the row goes at once (the panel's filter drops a row whose file is gone).

## What was found right

- **The eye analysis:** flat signals, a single edge, one or two points, NaNs, time
  backwards or repeated, and 2 million points (0.3 s), all handled. Bad `bit_period`,
  `levels`, `offset` and `level` are refused clearly. `edit_diagram`'s eye settings are
  checked. Eye diagrams with damaged saved fields (20 kinds) load and draw.
- **The PRBS source with ngspice:** the bad values refused at the run. A real PAM4 eye
  (four levels, three eyes). The capacitor note across it (50 A for 1 V in 20 ps; none
  invented for a parameter). The changed example `PRBS_eye_diagram.sch` simulates with no
  note.
- **Many nets with a real ngspice:**
  - 1000 nets with an equation (1002 variables), a parameter sweep, AC, FFT (1001-word
    `linearize`/`fft` lines, which ngspice takes);
  - a DC point printed in two parts (all 1000 node values right);
  - a transient with a DC point in one netlist.
- **The no-results rule:** none of the 88 examples fails with it. A dataset that is a
  symbolic link is written through to its target, the link kept.
- **The freeing:**
  - parts whose lists are rebuilt, through edits, undo, redo, replace and reopening, on
    ASan;
  - the Tuner rebinds by name after an undo;
  - the sweep, corners and optimizer blocks free the properties they replace;
  - no part is copied by value.
- **Undo:** a preview between edits; the saved mark through undo and redo; the selection
  kept across a preview, wires included; ground refs out of range refused.
- **The Projects menu:**
  - a plain folder, ".." and `user_lib` (no Open Project);
  - "Any folder is a project" (a plain folder opens);
  - the open project with unsaved changes unlinked through `context_menu` (the question,
    then the save dialog, nothing unlinked while it waits);
  - Open Project during a background run (the run's results said discarded, no crash).
- **Time:**
  - 30 document closes after a 10,000-part load, 0.06 s (as before);
  - edits of 10,000 parts with an eye diagram on, 0.43 s (as without);
  - `undo_history` of 20 steps at 10,000 parts, 1.9 s.
- **The fuzzers:**
  - `fuzz-sch` 280 mutants (560 runs);
  - `fuzz-simout` 150;
  - `smoke-test.sh hostile` 21 of 21;
  - the tool fuzzer 75,706 calls over three seeds, the server alive;
  - the monkey on five seeds (3 × 1500 steps on ASan, 2 × 3000 on the release build).

  No crash, hang or sanitizer report.

**Outside the scratch folder.** The smoke suite (`smoke-test.sh simulate`, 21:33) wrote 17
`spice4qucs.*` files into `~/Library/Caches/qucs-s/qucs-s`, though HOME, settings and
trash were isolated. The command line's work folder is the cache, which macOS finds
without HOME, and `QUCS_CACHE_DIR` was not set for that run. They were left there, and removed on 2 October by name, each dated 21:33 (a `log.txt` and a `spice4qucs.sw1.plot` of 29 September left as they were).

The system's Trash changed at 21:05, during the hunt. It cannot be listed, every run had
its own trash (`QUCS_TRASH_DIR`), and the user's own Qucs-S was running then.
`~/QucsWorkspace` changed only by that Qucs-S (`opamp741_prj`).

**Found while fixing (2 October).** Outside their folders, the tests also wrote:
- their documents' autosaves and an import's settings backups into `~/Library/Application Support/<the test>`. `useIsolatedSettings` now points both into the test's own folder.
- the library job's runs (`scripts/ci/test-library-parts.py`) into the real cache: it sets `QUCS_CACHE_DIR` and `QUCS_TRASH_DIR` now.
- `test_autosave` crashes a copy of itself on purpose (the crash handler's test), and macOS keeps a report of it in `~/Library/Logs/DiagnosticReports` and a count in `CrashReporter`. Left as it is.

## How to run it again

```
# a probe: an isolated --mcp-server, its workspace under the given folder
python3 p3_eye.py <folder>             # each p*.py takes the folder that holds probe.py and chain.py
# the tool fuzzer on the ASan build
ASAN_OPTIONS=detect_leaks=0 python3 p23_fuzz.py <scratch folder>/run 101 240
# the schematic and output fuzzers, as CI runs them (QUCS_CACHE_DIR set too)
HOME=<h> QUCS_SETTINGS_DIR=<s> QUCS_TRASH_DIR=<t> QUCS_CACHE_DIR=<c> \
  python3 scripts/ci/fuzz-sch.py <build-asan>/qucs/qucs-s.app/Contents/MacOS/qucs-s qucs-s-26.1.1/examples <out> --count 160 --seed 1001
python3 scripts/ci/check-tool-arguments.py
```

A1 by hand: a folder with a schematic (one labelled net, a `.TR`) and its dataset, the
folder `chmod 555`, ngspice replaced in the settings by a script that echoes a line and
exits 0; simulate, then compare the dataset with its copy.

The probes' notes, as taken: `notes.md`, beside this report.
