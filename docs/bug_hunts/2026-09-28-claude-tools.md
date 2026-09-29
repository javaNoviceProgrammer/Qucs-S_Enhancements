# Bug hunt: the Claude tools (the MCP server) and today's changes to them

*28 September 2026 - Qucs-S 26.1.4, `e6df622`. One hour, 14:05-15:05.*

Aimed at `QucsControl`, the 66 tools Claude (or any MCP client) drives Qucs-S with, which three
rounds of feedback reworked (`a8bad51`, `e6df622`) and no hunt had covered. Runs used
`qucs-s --mcp-server` (the same tools, no window). The ASan/UBSan Debug build was used for
crashes, the Release build for timings. Settings were isolated, with a scratch workspace, on
macOS arm64 with ngspice 46. Methods:

1. **A tool fuzzer** (`2026-09-28-claude-tools/fuzz_tools.py`). It makes random calls from each
   tool's schema, using the names of the parts open, ground refs and hostile values: empty and
   5,000-character strings, `GND#-1`, 2^31, 1e308, NaN-like text, arrays of 2,000. Three example
   schematics were open. It records a crash (EOF), a hang (no answer in 90 s) or a sanitizer
   report. There were 16 seeds and 9,650 calls: no crash, hang or UB report (section G).
2. **Pipelining**: an MCP client may send a request before the last one is answered.
   `pipe.py` sends groups of calls without waiting. `fuzz_pipe.py` is the fuzzer sending its
   random calls in pairs: 14 seeds and 11,600 calls, one UB report (E6, three times).
3. **Targeted probes** of what changed lately (ground refs, the typo check, previews, the
   untitled simulate, the file watch, net naming, import_netlist, expressions in plots), each a
   list of calls for `mcp.py`. The JSON-RPC layer was probed with `rpc.py`, and the JSON form
   with `roundtrip.py`: get_schematic's JSON form was set back into a new schematic for 35
   ngspice examples, and the netlists compared.
4. **Reading the code** of those paths. Entries found that way say so.

*Status:* all 32 fixed in `1be88f0`, and the minor notes (one was found right in this build). None
open. Each finding says what was done and which test holds it; each fix was broken on purpose to
see its test fail. After the fixes, the tool fuzzers on the ASan/UBSan build made 6,000 calls in
pairs and 6,400 one at a time with no hang, crash or UB report, and the JSON round trip gives the
same netlist for all 218 examples that netlist under ngspice.

| | severity | area | finding |
|---|---|---|---|
| A1 | high | export_netlist | `export_netlist` over the open schematic's own file destroys it on disk, and empties it in the window |
| A2 | high | preview, batch | A previewed batch puts every schematic back when it ends, undoing other calls that came in meanwhile |
| A3 | medium-high | batch, run_script, tune | A failing atomic batch, and tune, undo or overwrite edits of calls that came in while they ran |
| A4 | high | get_schematic json, set_schematic, add_component | Mirrored and turned parts come back turned 180°: the JSON form reverses sources and diodes |
| A5 | medium | preview, run_script | A preview (and a failed atomic script that changed nothing) throws away the redo steps |
| A6 | medium-high | import_netlist | import_netlist changes the circuit: node names split by case, `n+1` merged with `n_1`, the first element eaten as the title |
| A7 | medium | simulate | `keep_as` overwrites another schematic's dataset |
| A8 | medium | import_netlist | Two imports of one title share `<title>_subcircuits.lib`: the second changes the first's subcircuits |
| A9 | medium-high | batch | A batch's calls without a path follow "the document in front": when that changes mid-batch, the rest land in another document |
| A10 | medium | save_document, File > Save As | A Save As that fails leaves the document named after the file it could not write: every save after fails too |
| A11 | medium | open_document (and the window) | One file opens as several documents - through a symlink, or in another case on a case-insensitive disk |
| A12 | low-medium | create_subcircuit (today) | A file it may write but not read is deleted when the tool fails or is previewed - "is as it was", it says |
| B1 | high | get_dataset | A deeply nested expression crashes Qucs-S (stack overflow in the dataset parser) |
| B2 | high | get_dataset | `eye` with a tiny `bit_period` never ends and eats all memory (13 GB in 10 min) |
| B3 | medium | arrange | arrange is quadratic: a minute of frozen window at 3,000 parts |
| B4 | high (headless) | open_document | Opening a read-only file, or one of a newer version, hangs the MCP server for good (a modal box) |
| C1 | medium-high | set_label, rename_net | A net named `0` or `GND` is shorted to ground, silently |
| C2 | medium | set_label, rename_net, edit_component | Net and part names are not checked as the GUI checks them: spaces, `;`, `$` break the netlist |
| C3 | low-medium | rename_net, get_schematic | A net renamed to `net2` while another net is `net2`: two nets of one name |
| C4 | low-medium | create_subcircuit (today's refs) | Grounds alone made into a subcircuit: the circuit loses its ground, silently |
| D1 | medium | typo check (today) | The typo check refuses real units (`1 kOhms`, `5 Volts`, `10 dBV`) and blocks unrelated `.sch` text edits |
| D2 | low-medium | file watch (today) | A file dated in the future is loaded again just after it is opened, and told as changed on disk |
| D3 | low-medium | add_diagram, add_trace | Expressions in a diagram's traces are still dead traces (fixed for add_analysis alone) |
| D4 | low | add_analysis (today) | Expressions in `plot` are written into the NutmegEq unchecked (`x=1`, `v(out) +`) |
| D5 | low | add_painting | The schema has no `tex` (a formula's text), and today's description of `text` says it is |
| E1 | medium | simulate | No analysis block: "Failed to start simulator: Unknown error" |
| E2 | low-medium | Claude Code dock | A sign-in failure shows Claude Code's text alone; `/login` cannot run in the dock |
| E3 | low | add_marker | `at` past the curve is put on its last sample; `label_offset` overflows |
| E4 | low-medium | simulate | A simulation whose document is closed while it runs answers with another run's log, or with no reason |
| E5 | medium | add_component, find_library_component | A library part cannot be placed: "There is no component type Lib", though find_library_component says to |
| E6 | low | diff | `diff steps: -2147483648`: signed overflow (UBSan) before the range check |
| E7 | medium | simulate | A dataset that cannot be written: no error in the answer - the reason is in a modal box |
| F | - | minor notes | Fourteen small ones (section F) |

---

## A. Data lost or corrupted

### A1. `export_netlist` over the open schematic's own file destroys it on disk, and empties it in the window

**Fixed in `1be88f0`.** `export_netlist` refuses a file open in Qucs-S ("... is open in Qucs-S
(victim.sch): the netlist would be written over it") and, unless `replace` is given, a file of
Qucs-S's own kinds: a schematic, symbol, data display, dataset, Verilog-A or VHDL file, known by its
suffix or a first line `<Qucs `. A netlist's own file is still written over. Test:
`test_qucs_control` `aNetlistDoesNotReplaceADocument`.

`export_netlist` "writes over an existing file" and does not look at what that file is. With
`save_as` the path of the schematic open in front, it writes the netlist over the `.sch`. The
document has no unsaved changes, so the file watch loads it again about 1.5 s later. The netlist
does not read as a schematic, so the window is left with an empty schematic (0 components) and a
modal "Wrong document type" box. The circuit is gone from disk, and from the window until the box
is answered: undo then brings it back (`victim4.json`). The file on disk stays a netlist until the
schematic is saved again, and closing the tab first loses it for good.

```
open_document victim.sch; export_netlist {"save_as": ".../victim.sch"}
-> {"format":"spice","lines":16,"written":".../victim.sch"}
4 s later: get_state -> "dialog open":"QMessageBox", victim.sch "components":0
```

`victim2.json`. Any `.sch`, `.dpl` or `.sym` is at risk, open or not: a schematic that is not
open is overwritten without a word. *Fix:* refuse a target that is an open document's file, or a
file of Qucs-S's own kinds, unless `replace` is given (`copy_document` and `create_subcircuit`
already refuse "the schematic itself" / "is open").

### A2. A previewed batch puts every schematic back when it ends, undoing other calls that came in meanwhile

**Fixed in `1be88f0`.** Some calls run over several turns of the event loop and put things back, or
must not have other changes land in their midst: `batch`, `run_script`, `tune` and any call with
`preview`. These now run alone (`QucsControl::callToolFor`, `runsAlone()`). Calls that come
meanwhile wait in a queue and run in turn after, whether pipelined by the same client or sent by
another conversation. `get_dialog` and `set_dialog` do not wait, since a dialog may be what the call
waits for. A preview's put-back of files no longer meets another call's file either. Test:
`callsWaitForOneThatRunsAlone` (a previewed batch with a call behind it: that call's part stays,
with its step to undo).

`preview` snapshots every open schematic, runs the tool, then restores them all and cuts their undo
stacks back (`forgetUndoAfter`). A `batch` is asynchronous: its calls run one per event-loop turn.
A request that arrives meanwhile runs in between. That may be pipelined by the same client, or come
from another conversation in the dock. Its change then goes in the preview's rollback.

```
[batch {calls: 3 x add_component, preview: true}] + [add_component C_OTHER]   (sent together)
-> add_component: {"name":"C_OTHER", ...}        (success)
-> get_schematic overview: "components":0 ; undo_history: "steps":[]
```

`race1.json`. The same global `a_previewing` counter would also let a real `create_subcircuit`,
run in between, have its file put back (deleted) by the preview (by reading). *Fix:* run a
previewed batch synchronously, or refuse other calls while a preview is under way (queue them), or
restore only what the preview's own calls changed.

### A3. A failing atomic batch, and tune, undo or overwrite edits of calls that came in while they ran

**Fixed in `1be88f0`.** The queue of A2 keeps other tool calls out of an atomic batch's rollback and
out of `tune`. `tune` also leaves a value the user changed in the window while it ran, and says so:
"R of R1 was changed to 47k while it was tuned: left so, not put back to 1k, and nothing applied
over it". The atomic batch names an untitled schematic by its tab ("untitled as it was"). It
compares with the value it set last, a failed run's too. Tests: `callsWaitForOneThatRunsAlone` (a
failing atomic batch with a call behind it), `aPartIsTunedToATarget` (a value changed in the window
between two runs).

The same interleaving hits the other two tools that put state back:

- **Atomic `batch`**: on failure it restores every schematic whose revision changed since the batch
  began. That includes a change another call made in between: undo_history shows
  "RB1 deleted; C_OTHER deleted" (`race2.json`). One undo brings it back, unlike A2. The message
  also names an untitled schematic as "" ("undone -  as it was").
- **`tune`** sets the property for each run and puts the value it had at the start back at the end.
  An `edit_component R1 R=47k` sent while it ran was answered as done, then overwritten: "R is 1 kOhm
  as it was". The edit landing mid-run also broke the measurement: "the operating point has no out
  (its nodes: )" (`race3.json`).

*Fix:* as A2: queue other calls while a batch, a script or a tune runs, or compare each
schematic to what the tool itself left.

### A4. Mirrored and turned parts come back turned 180°: the JSON form reverses sources and diodes

**Fixed in `1be88f0`.** `add_component`, `set_schematic`'s JSON form and `replace_component` now
mirror and turn a new part in the order a file is read (`orient()`: mirrored, then turned to its
count), as `edit_component` already did; get_schematic reads back the rotation and mirroring given.
The JSON form also carries what it lost:
- a property that comes any number of times, as a list of its values: NgSweep's Record and Vs, a
  Monte Carlo's or corners' Record and Spec, NgOpt's Knob and Target, an optimization's Var and
  Goal;
- the properties a type makes only once the part is made again: an EDD's I2 and Q2 (with its
  Branches), a subcircuit's parameters (with its File; taken as given when the file is not found);
- the properties of a type that takes more by name, such as a .MODEL's Line_6;
- a label's initial value, as `initial` (its `.IC`);
- two names a file holds in two cases when one part is a simulation block (a transformer Tr1 and
  a transient TR1).

The round trip was run on every example: all 218 schematics that netlist under ngspice, set back
beside themselves, give the same netlist line for line. Before, 25 of 35 differed. Test:
`theJsonFormMakesTheSameNetlist` (eleven examples, one of each kind).

get_schematic reports orientation as `rotation` (quarter turns from the type's own) and
`mirror`/`mirrored`. add_component, set_schematic's JSON form and replace_component take the same
names, but apply them in another order. A part both mirrored and turned comes back turned 180°, its
pins swapped. For a source or a diode, the polarity reverses.

```
file: <Vac V1 1 30 130 -78 -26 1 1 ...>      -> get_schematic json: {"mirror": true, "rotation": 0, ...}
add_component Vac {"rotation": 0, "mirror": true} -> reads back rotation 2, mirrored; pin 1 at the bottom
```

`roundtrip.py` (seed 1) made the JSON form of 35 ngspice examples and set it back into a new
schematic. **25 of the 35 netlists differ**:
- polarity: `V1 in 0` became `V1 0 in` (RCL_resonance, RC_filter_FFT, active_bp, ...) and
  `D1 Vd 0` became `D1 0 Vd`;
- pin order: EDD equations negated;
- four JSON forms were refused: an EDD's extra branches (`I2, I3, Q2, Q3`), NgSweep's `Record`,
  a `SUB` instance's parameters (`Cs, Lq, f`), and type `Lib` ("there is no component type Lib");
- a net label's initial value is left out of the JSON form, so `.IC v(TOUT)=15 V` was lost
  (sawtooth-2).

`edit_component` does it right: `rotation: 0, mirror: true` on a Vac gives the file's layout, pin 1
on top, while `add_component` with the same values gives rotation 2, pin 1 at the bottom
(`mir2.json`); `replace_component` with them gives rotation 2 too (`mir3.json`). `mir.json`. *Fix:* apply mirror and rotation in the order Qucs stores them (as `Component::load`
does), and make the JSON form carry what `componentModel()` leaves out. Then test the round trip
on every example.

### A5. A preview (and a failed atomic script that changed nothing) throws away the redo steps

**Fixed in `1be88f0`.** A preview, and an atomic script that fails, put the undo stacks back whole
(`Schematic::undoStacks()` / `setUndoStacks()`; the steps are shared, not copied). The redo steps
that the preview's first step cut off come back, and so do the oldest steps a full stack dropped.
Test: `aPreviewKeepsWhatRedoWouldDo`.

Add R1, add R2, undo, then any tool with `preview: true`. The redo step is gone ("There is nothing
to redo"), although the preview says nothing is changed. `forgetUndoAfter(marks)` cuts at the
undo position, not the stack's end. A `run_script` with `atomic` that throws does the same, even when
it changed nothing. The atomic batch checks the revision first and keeps it. `redo.json`, `redo2.json`.

### A6. import_netlist changes the circuit: node names split by case, `n+1` merged with `n_1`, the first element eaten as the title

**Fixed in `1be88f0`.** The import now keeps the netlist's nodes:
- nodes that differ only in case are one net, named as first written;
- a name made safe for a label (`n+1` as `n_1`) takes a suffix when another node has that name
  (`n_1_2`); nodes whose names are already safe keep them;
- the title is the file's first line itself, a `; comment` included, not the first line left once
  comments are gone;
- a second element of one name is placed as `R1_2`, and "not taken" says so.

Test: `aNetlistImportKeepsItsNodes`.

- **Case**: SPICE node names are case-insensitive. `r1 In Out 1K` / `c1 out 0 1U` came in as three
  nets, `In`, `Out` and `out`, so C1 is cut off from R1.
- **Names made safe collide**: `n+1` becomes `n_1`. A netlist that also has `n_1` gets the two
  merged, so V1 (1 V) and V2 (2 V) end up in parallel on one net, silently (`imp2.json`, collide).
- **Title**: a first line `; comment` is skipped as a comment, and then the first element,
  `R1 a 0 1k`, is taken as the title and lost. The title check reads the first raw line, the
  title is taken from the first netlist line.
- **Duplicates**: two elements named R1 are placed as two R1s, which ngspice refuses. Nothing
  is said.

`imp.json`, `imp2.json`.

### A7. `keep_as` overwrites another schematic's dataset

**Fixed in `1be88f0`.** `keep_as` is refused before the run when a schematic of that name is beside
this one ("'keep_as' rc would write over the dataset of .../rc.sch"). The copy step checks it again.
Test: `keepAsIsNotAnotherSchematicsDataset`.

`simulate` with `keep_as: "rc"` writes `rc.dat.ngspice` beside the schematic. If `rc.sch` is there,
its dataset is replaced by this run's without a word (md5 changed; `keep.json`). Its diagrams then
show another circuit's results. *Fix:* refuse a name that is a schematic's there, or prefix kept
runs (`name.run.dat...`).

### A8. Two imports of one title share `<title>_subcircuits.lib`: the second changes the first's subcircuits

**Fixed in `1be88f0`.** An import writes its subcircuits into a file of its own:
`<title>_subcircuits.lib`, else `-2`, `-3` and so on. A file already there is reused only when it
holds the very same subcircuits. Test: `aNetlistImportKeepsItsNodes`.

Without `save_as`, the subcircuits of an import go into `<title>_subcircuits.lib` in the workspace.
A second netlist of the same title overwrites it. The first schematic, still open, includes that
file by path, so its `X1` silently becomes the second netlist's `blk`, an R turned into a C
(`lib2.json`). *Fix:* a unique name (as with `save_as`), or refuse to write over one.

### A9. A batch's calls without a path follow "the document in front": when that changes mid-batch, the rest land in another document

**Fixed in `1be88f0`.** The queue of A2 keeps other calls out of a batch. Within a batch, a call
without `path` acts on the document the batch's own calls left in front. If the user brings another
document to the front meanwhile, the batch's document is put back. If it was closed, the call is
refused rather than run on another document. Test: `callsWaitForOneThatRunsAlone` (the user's click
between two of the batch's calls).

A batch runs its calls one per event-loop turn. Each call without `path` acts on the document in
front *when it runs*. A `close_document` sent behind a batch of three `add_component` closed the
untitled schematic after the first. The other two, CB2 and CB3, were added to plain.sch, the
document then in front. Every call answered as done (`close.json`, group 5). In the window, the
user switching tabs during a batch does the same. *Fix:* resolve the batch's document once, when
it starts, for the calls that give no path.

### A10. A Save As that fails leaves the document named after the file it could not write: every save after fails too

**Fixed in `1be88f0`.** When the save fails, `QucsApp::saveDocumentAs` gives back the old name, file
info, dataset and data display names, tab text and last folder. The window's Save As goes through it
too. Test: `aFailedSaveAsKeepsItsName`.

`QucsApp::saveDocumentAs` gives the document its new name (`Doc->setName(s)`) before it saves,
and does not give the old one back when the save fails. `save_document as: /System/nope.sch` on
tn.sch with an edit answers "could not be saved". The document is now `nope.sch`, at
`/System/nope.sch`, unsaved. A plain `save_document` then fails again, and closing asks to save or
discard: the edit can reach tn.sch only by another Save As (`failsave.json`). The window's Save As
shares the function. *Fix:* keep the old name, file info and tab text when `save()` fails.

### A11. One file opens as several documents - through a symlink, or in another case on a case-insensitive disk

**Fixed in `1be88f0`.** `QucsApp::findDoc` finds a document by its name as given, then by the file
itself (`misc::isSameFile`: device and inode), so `gotoPage` brings the open one to the front. The
tools' `sameFile` compares the files too. Test: `oneFileIsOneDocument` (through a symbolic link, and
in another case where the disk ignores case).

`open_document` of `ws/tn.sch`, then of `wslink/tn.sch` (a symlink to `ws`), then of
`ws/TN.sch` (APFS ignores case) gives three documents of one file (`twice.json`; `../` is caught).
Each keeps its own edits, and the last one saved writes over the others. get_state lists
`tn.sch` twice, so the bare name is ambiguous too. `openDocument` compares with `sameFile`,
but `gotoPage` finds an open document by its exact name, and opens a new one. *Fix:* find open
documents by canonical path (as `reloadChangedFiles` already does).

### A12. A file it may write but not read is deleted when the tool fails or is previewed

**Fixed in `1be88f0`.** `create_subcircuit` refuses to replace a file it cannot read ("... is there
and cannot be read (...): it would be written over with no way back"). Test:
`anUnreadableFileIsNotWrittenOver`.

Today's put-back keeps what the file held by reading it first: `if (old.exists() && old.open(ReadOnly))`.
A file that can be written but not read (mode 200) reads as absent. The tool then writes it
(`replace`), and on failure, or at the end of a preview, puts back "no file", which removes it.
A preview of `create_subcircuit save_as: wosub.sch replace: true` over such a file answered "nothing
changed, and wosub.sch is as it was", and the file was gone (`wo.json`). Its subcircuit also had 0
pins, since the file written could not be read back. *Fix:* refuse to replace a file that cannot be
read, or keep "unreadable" apart from "absent".

## B. Crashes and hangs

### B1. A deeply nested expression crashes Qucs-S (stack overflow in the dataset parser)

**Fixed in `1be88f0`.** The dataset parser refuses an expression nested more than 200 levels deep
("it is nested too deeply (more than 200 levels)"). It counts its own recursion (parentheses, signs,
functions) and also the depth of the tree it builds, since evaluating and freeing the tree recurse
too: `a+a+a...` is as deep as it is long. `dataset::checkExpression()` checks an expression's syntax
alone (used by D4). get_dataset's error shows a long expression by its ends. Test: `test_dataset`
`aDeepExpressionIsRefused`.

`get_dataset` evaluates expressions with a recursive-descent parser (`dataset.cpp`, `Parser`).
`"-" * 20000 + "v(out)"`, or 100,000 parentheses, overflow the stack. The ASan build reports
`AsanOnDeadlySignal` in `Parser::skip`/`take` (crash report `qucs-s-2026-09-28-141623.ips`); the
Release build dies as well. One tool call takes the whole window down, and any unsaved work with
it. `deep2.json`. *Fix:* a depth limit in the parser (refuse beyond, say, 200 levels).

### B2. `eye` with a tiny `bit_period` never ends and eats all memory

**Fixed in `1be88f0`.** An eye whose bit is shorter than a sample is refused ("... bits in the
range, more than its N samples: the bit period is too short"). Its loop counts bits (`start +
(centre + k)·T`) instead of adding T to t. Test: `test_dataset` `anEyeOfNoSamplesIsRefused`.

`get_dataset {measure: ["eye"], bit_period: 1e-300}` loops at `dataset.cpp:1426`,
`for (t = start + centre*T; t <= c.x.last(); t += T)`. Once `t + T == t`, it never advances, and
`highs`/`lows` grow for good. It reached 13 GB RSS after 10 minutes and was killed (`meas.json`,
call 2). `thd` with extreme fundamentals is refused properly. *Fix:* refuse a `bit_period` that
gives more bits than samples (or more than, say, 1e6).

### B3. arrange is quadratic: a minute of frozen window at 3,000 parts

**Fixed in `1be88f0` and `1e90dd8`.** `joinPieces` now wires all the nets in pieces at once: each
net's two closest pieces, by the first clear way. It checks the nets once for the lot, and goes back
to one wire at a time only when that check finds a fault. What each wire looked through is now found
by place: `clearWay()` looks in an index of the nodes, parts and wires (`WayIndex`); the wiring and
the parts put back run inside the schematic's `IndexedInsertion`, which now also finds the nodes a
new wire crosses and forgets a wire `installWire()` deletes; and the parts are lifted off in one
pass (`Schematic::detachComps()`). arrange now takes time in proportion to the parts: 2,500 in 0.2 s
and 20,000 in 2 s (3,000 took 59.5 s before). Test: `aLargeSchematicIsArrangedInTime` - four times
the parts in under eight times the time (800 and 3,200 parts; sixteen times without the batching),
whatever the machine's speed. The indexes are measured rather than tested: at 20,000 parts they took
the time from 11.7 s to 2 s.

`arrange` (preview) of a grid of resistors in chains took 0.54 s at 300 parts, 5.7 s at 1,000
and 59.5 s at 3,000. It succeeds, but the window is frozen for a minute and says nothing
(`big300.json`, `big1000.json`; the 3,000-part case is the same grid, 60 to a row). By reading: each wire's way is checked with a full `netsOf()` of the schematic.

### B4. Opening a read-only file, or one of a newer version, hangs the MCP server for good (a modal box)

**Fixed in `1be88f0`.** No message box can hold up a tool call now (`QucsControl::eventFilter`). A
box that opens during a call is one no one answers until the call ends, and under `--mcp-server` no
one at all. In the window, a box the user opens while a call waits in an event loop of its own (a
script waiting on a simulation, a question to the user) is theirs and left alone. A call's box is
closed with its safe button (No, Cancel or OK), and what it said goes into the answer: "Qucs-S
reported: ... (a message box, answered No)". The boxes found are also gone at their source:
`open_document` says in its answer that a file or its folder is read-only; a newer version's file
opens with a note; and a data display that cannot be made says why, with no tab of no file left
behind. Test: `noMessageBoxWaitsOnACall`.

`gotoPage` warns with a modal `QMessageBox::warning` ("Document opened in read-only mode!
Simulation will not work...") for any file that is not writable. A file of a newer Qucs version
raises a box as well. Under `qucs-s --mcp-server` nobody can answer it, so `open_document` of
`chmod 444 tn.sch`, of `/bin/ls`, or of a schematic headed `<Qucs Schematic 99.0.0>` never
answers. The server is stuck for good (`ro.json`, `modal.json`). In the window, the tool call waits
until the user clicks OK. The examples inside an installed app bundle are read-only, so opening one
hangs a headless server. `new_document kind: data_display` for a schematic in a read-only folder
hangs the same way: its new `.dpl` cannot be written (`dpl.json`). *Fix:* no box while a tool call runs (`ErrorCapture::active()`): say it in
the answer, as the other load errors are.

## C. The circuit changed by a name

### C1. A net named `0` or `GND` is shorted to ground, silently

**Fixed in `1be88f0`.** `set_label` and `rename_net` refuse ground's names, 0 and gnd in any case:
"gnd is ground's name: a net so called is ground in the netlist, shorted to it (connect with
'ground' grounds a pin, if that is meant)". Test: `namesAreCheckedAsTheDialogsCheckThem`.

`rename_net out -> 0` is accepted, and the netlist reads `R2 0 0 1K`: the net is ground now.
`-> GND` is accepted too, and ngspice takes `gnd` as ground. `set_label 0` on a source's pin gives
`V1 0 0 DC 1`. check_schematic says nothing. rename_net refuses a name another net has, but does
not count ground's names. `gndname.json`.

### C2. Net and part names are not checked as the GUI checks them: spaces, `;`, `$` break the netlist

**Fixed in `1be88f0`.** Net names are checked as the label dialog checks them: a letter, then
letters, digits and single `_`. Part names given to add_component, edit_component, replace_component
and create_subcircuit are checked as the component dialog checks them: a letter, then letters,
digits and `_`. The JSON form still takes the names files hold, such as 2N2222, but not what breaks
a line (spaces, quotes, `<`, `>`). Test: `namesAreCheckedAsTheDialogsCheckThem`.

The label dialog allows `[a-zA-Z]([0-9a-zA-Z]|_(?!_))+`; the tools take anything.

- `set_label "a 0"` gives `R1 _net0 a 0 1K`, an extra node.
- `x;y` and `n$1` begin ngspice comments. `a=b`, `{a}` and `v(x)` are read as other things.
- `edit_component rename`: `R 9` gives `R 9 _net0 0 1K`. `R;9` and `R$9` begin comments.
  `R9.2` makes its pins `R9.2.1`, which connect cannot tell. `9R` is netlisted as `R9R`.
  `add_component` names the same.

`labels.json`, `names.json`. *Fix:* the dialog's rule for labels, and SPICE-safe part names, in
the tools.

### C3. A net renamed to `net2` while another net is `net2`: two nets of one name

**Fixed in `1be88f0`.** `net1`, `net2` and so on, the names get_schematic gives nets without a
label, are refused as a label's or a rename's. get_schematic's description says they are numbered in
order and may change. Test: `namesAreCheckedAsTheDialogsCheckThem`.

`rename_net net1 -> net2` is accepted while get_schematic calls another net `net2`. The next read
lists two different nets named `net2`, and `rename_net from: net2` takes the labelled one. The
automatic names also shift after any label, so a name read before now means another net
(`netname.json`). *Fix:* refuse `net<n>` names, or number automatic nets so they cannot collide
(`_n1`).

### C4. Grounds alone made into a subcircuit: the circuit loses its ground, silently

**Fixed in `1be88f0`.** Grounds named for a subcircuit stay where they are, and the answer says so:
ground is one node everywhere, and the parts inside on it get grounds of their own. A group of
grounds alone is refused. Test: `namesAreCheckedAsTheDialogsCheckThem`.

With ground refs, `create_subcircuit names: ["GND#1", "GND#2"]` is taken. The two ground symbols
move into `grounds_only.sch`, a subcircuit with no ports ("grounds inside": 2), and `SUB1` takes
their place. The top schematic has no ground symbol left, so V1 and R2, which stood on them,
float (`cs.json`, overview: no GND). *Fix:* refuse a group that carries no part with a pin
other than a ground, or leave the grounds behind.

## D. Today's changes (`e6df622`)

### D1. The typo check refuses real units, and blocks unrelated `.sch` text edits

**Fixed in `1be88f0`.** The unit grammar takes plurals and spelled-out units (Ohms, Volts, Amps,
Hertz, Farads ...), dB of any reference (dBV, dBc, dBi, dBmV, dBFS), degC and degF, and a rate
(V/us). `1kk` and `10uu` are still refused. `set_schematic`'s text form checks parts only when its
text gives them, and only values that changed: a part that keeps its old value is not the text's to
answer for. Test: `realUnitsAreNoTypos` (a text of paintings alone; a text giving the parts again,
one keeping its old value, one new with `2kk`).

The new check refuses a number followed by letters that are not a scale and a unit. Its unit list
is too narrow: `1 kOhms`, `5 Volts`, `2 Amps`, `1 Hertz`, `10 dBV`, `3 dBc`, `25 degC` and
`1.5 V/us` are all refused (`typo.json`). In set_schematic's text form, the check runs over *every*
component after the text is read, not only the lines given. On a schematic that holds `1 kOhms`
(`legacy.json`), a text of `<Paintings>` alone is refused. *Fix:* plurals and spelled-out units,
units with a slash and dB variants; check only the components in the text given.

### D2. A file dated in the future is loaded again just after it is opened, and told as changed on disk

**Fixed in `1be88f0`.** A document remembers its file's own date, taken before the file is read,
rather than the time of the load (schematics and texts). A file dated in the future is not loaded
again. The small oddity was that a simulation block's text is placed as it is drawn
(`SimulationComponent::drawSymbol`), so every load "moved" it; the change reports now leave that
out. Test: `aFileFromTheFutureIsNotLoadedAgain`.

The watch now looks at a file when its watch is set, and loads it again if the file is newer than
its last load. A file whose date is in the future (clock skew, a file unpacked from an archive)
is always newer. Opened with `open_document`, it was loaded again ~1.5 s later. Claude was told
"future.sch was changed by its file, changed on disk ... What changed: DC1: its text moved"
(`fut.json`). That the second load differs from the first is a small oddity of its own. *Fix:*
compare the file's date with the date it had when loaded, not with the time of the load.

### D3. Expressions in a diagram's traces are still dead traces

**Fixed in `1be88f0`.** `add_diagram`, `add_trace` and `edit_trace` now trace an expression
(`ac.db(v(out))`, `v(out)*2`) as add_analysis does: as a variable of a NutmegEq run after the
analysis. That is an equation of one already there that computes it, else a new equation of the
analysis's NutmegEq, or of a new NutmegEq beside the analysis when it has none (one step to undo,
said in the note); add_analysis shares that block too. The analysis is the one the name gives, else
the schematic's only AC or transient analysis. Under Qucsator and Xyce the call is refused and says
how to do it. After a run, ngspice writes a computed vector that keeps a voltage's type as `v(name)`
(`mag(v(out))` as `ac.v(mag_v_out)`): a trace named `ac.mag_v_out` finds it so
(`Graph::loadDatFile`). Tests: `anExpressionTraceIsComputed`, `aComputedVoltageIsFound`.

Today's fix for `plot` covers add_analysis alone. `add_diagram` and `add_trace` still take
`ac.db(v(out))`, `ac.v(out)/2` and `ac.mag(v(out))`. They say "no variable ... until a simulation
writes it", and after a successful run they are still listed under "traces without data"
(`trace.json`): the dead end the feedback reported. *Fix:* the same NutmegEq, or refuse with the
way to do it.

### D4. Expressions in `plot` are written into the NutmegEq unchecked

**Fixed in `1be88f0`.** `add_analysis` reads each expression of `plot` (`dataset::checkExpression`)
before anything is added: `x=1`, `v(out) +` and `2e` are refused, with why. The trace tools do the
same (D3). Test: `anExpressionTraceIsComputed`.

`plot: ["x=1", "v(out) + "]` gives equations `x_1=x=1` and `v_out_2=v(out) +`. They fail only at
the next simulation, far from the call that made them (`typo.json`, last call). `2e` is also
taken as a number (E, exa).

### D5. The schema has no `tex` (a formula's text), and today's description of `text` says it is

**Fixed in `1be88f0`.** The schemas of add_painting and edit_painting list a formula's `tex` and
`display`; `text` is a text's or text box's. Test: `anExpressionTraceIsComputed` (checked through
describe_tool).

A formula painting takes `tex` (and `display`), which add_painting's schema does not list.
Today's field description of `text` says "a formula's text (TeX for a formula)". Following it
gives "A formula has no text; it takes: angle, color, display, size, tex, x, y."

## E. Wrong behaviour

### E1. No analysis block: "Failed to start simulator: Unknown error"

**Fixed in `1be88f0`.** `simulate` refuses a schematic with no analysis before anything else, and
says so: "... has no analysis to run (no .AC, .TR, .DC, .SP ... block): add_analysis adds one". An
untitled schematic is not saved for nothing. Test: `simulateSaysWhatIsWrong`.

`simulate` of a schematic with no analysis, saved or untitled, answers "The simulation did not start.
Failed to start simulator \"/Users/meisam/bin/ngspice\": Unknown error". The reason, "no simulation:
no .AC, .TR, .DC ... block", comes only among the Check Schematic warnings after it (`noan.json`).

### E2. A sign-in failure shows Claude Code's text alone; `/login` cannot run in the dock

**Fixed in `1be88f0`.** When a turn fails on its sign-in (the text says authenticate, /login, API
key, OAuth, 401 or credentials), the dock adds: "To sign in again, run claude in a terminal and type
/login there (the dock cannot run /login), then send the message again." Test: `test_claude_code`
`theStreamBecomesSignals`.

A turn that fails shows `failureOf(subtype, text)`: Claude Code's own message, "Failed to
authenticate" or "Please run /login". `/login` does not work in the dock (a print-mode session),
and nothing says to sign in with `claude` in a terminal. A user asked what to do on 28 September
(by reading `claudecodepanel.cpp` `onTurnFinished`, `claudecode.cpp` `handleResult`).

### E3. `at` past the curve is put on its last sample; `label_offset` overflows

**Fixed in `1be88f0`.** A marker's `at` off its trace is refused, with the trace's x range.
`label_offset` takes offsets within 10,000, and a `label` place is worked out in 64 bits. Test:
`test_qucs_control`'s marker cases (`at` 1e308, an offset of 2^31 - 1).

`add_marker at: 1e308` on a 1 ms transient puts the marker on the last sample, time 0.001, and
says nothing. `label_offset [2^31-1, 2^31-1]` is accepted: the label lands at
`[-16777076, 16777896]` (x's sign flipped), and the diagram's screenshot becomes 10,001 × 9,999
pixels (`mk.json`).

### E4. A simulation whose document is closed while it runs answers with another run's log, or with no reason

**Fixed in `1be88f0`.** `simulate` starts the simulator on its own schematic, brought to the front,
not on whatever document is in front when the run starts. It answers "... was closed before its
simulation began: nothing was simulated", or "... was closed while it was simulated: its results
were discarded". Test: `simulateSaysWhatIsWrong`.

`simulate` of rc.sch, with `close_document` sent right behind it, once answered with a log of
plain.sch, headed "qucs 26.1.3" (a stale log), with no dataset. Another time it answered "The
simulation did not start." with no reason (`close.json`, `close2.json`). The answer should say the
document was closed.

### E5. A library part cannot be placed: "There is no component type Lib", though find_library_component says to

**Fixed in `1be88f0`.** `newComponent()` makes a `LibComp` for type Lib, as the file loader does;
the library part's parameters come with its Lib and Comp (A4). A library part that is not found (its
library, or the part in it) is refused rather than placed with no pins. The loader's other hand-made
types were in reach already: Eqn and SPICE, while `Rus` and `I` are old names of R and I(SFFM).
Tests: `aLibraryPartIsPlaced`, `theJsonFormMakesTheSameNetlist`.

find_library_component's answer says a Qucs library part is placed with `add_component` of type
`Lib`, with its `Lib` and `Comp`. add_component, describe_component_type and set_schematic's JSON
form all answer "There is no component type Lib". `newComponent()` looks the type up in the
module registry, where `Lib` is not: `getComponentFromName()` makes a `LibComp` for it by hand
(`lib3.json`; also seen in A4's round trip). A library part can be placed only through
set_schematic's `.sch` text. *Fix:* make `Lib` in `newComponent()` as the loader does (and check the loader's other
hand-made types the same way).

### E6. `diff steps: -2147483648`: signed overflow (UBSan) before the range check

**Fixed in `1be88f0`.** `diff` checks `steps` before subtracting it. Test: `simulateSaysWhatIsWrong`
(INT_MIN is refused; the ASan/UBSan build runs it).

`diffTool` computes `const int at = sch->undoIndex() - steps;` and checks `steps < 1` only after it.
Given INT_MIN, it overflows: UBSan's `qucscontrol_resources.cpp:367:41: runtime error: signed
integer overflow: 2 - -2147483648`. Found by the pairs fuzzer (`fuzz_pipe.py`, seeds 43, 44 and 54).
The answer is still the refusal. *Fix:* check `steps` first.

### E7. A dataset that cannot be written: no error in the answer - the reason is in a modal box

**Fixed in `1be88f0`.** Writing the dataset returns its error
(`AbstractSpiceKernel::convertToQucsData`). The run keeps the error
(`SimulationRun::datasetError()`) and fails. It shows a box in the window only, never for a tool's
run (`setQuiet`). `simulate` puts the error in `errors`, and `dataset written` goes by the file's
date before the run (newer than it, or no file before) rather than the second the run began. Test:
`aDatasetThatCannotBeWrittenIsSaid` (with ngspice, where there is one).

With the schematic's dataset file read-only, `simulate` answers `"dataset written": true`,
`"errors": []`, `"finished": false` and no `succeeded`. The file is unchanged: still 2 bytes.
The reason comes as a modal box, "Failed to create dataset file ... Check write permission of the
directory". Under the MCP server it then blocks the next calls, like B4 (`rods.json`).
The file had been made in the same second as the run began, and "dataset written" goes by the
file's date, so its `true` may be that; the missing error does not depend on it. *Fix:* say the
error in the answer, with no box while a tool call runs, and set "dataset written" from the
conversion's own result.

## F. Minor notes

- Recent Documents fill up with the scratch files untitled simulations are saved as, and the
  `untitled-N.sch` accumulate in the scratch folder (the fuzzers left 48 files in
  `~/Library/Caches/qucs-s/projects/ws-…`). *Fixed:* a schematic saved in the scratch folder to be
  simulated is no longer one of Recent Documents (`QucsApp::forgetRecentFile`). The files stay: each
  is a schematic someone made, in a cache folder.
- `set_label` with `at: null`: " is not a pin ..." (the empty name leads the message). *Fixed:*
  "Which place? A pin (\"R1.2\") or a place ([x, y])."
- JSON-RPC: a request array is refused as "Parse error: no error occurred"; `id: null` is answered
  with id 0; `params: [1, 2]` gives "There is no tool ." rather than -32602 (`rpc.py`). *Fixed:* a
  batch is refused as one (-32600, "a batch of requests is not taken - one request a line"); an id
  comes back as it was, null too; params that are not an object, a tool's name that is not a string,
  and arguments that are not an object are invalid params (-32602); an id that is an object is an
  invalid request. Tests: `test_mcp_server` `badRequestsAreSaidSo`, `theProgramServesOverStdio`.
- `move_to_pane pane: 1.5`: "There is no pane 0". *Fixed:* "There is no pane 1.5".
- `screenshot region [-2^31, -2^31, 2^31-1, 2^31-1]`: "nothing ... at the region", although the parts
  are inside (the rectangle's width overflows). *Fixed:* a region's corners are kept on the canvas
  before its size is taken (`regionOf()`), for get_schematic too.
- `add_wire [[0,0],[0,0]]`: "Wire drawn through 2 places", and nothing is drawn. *Fixed:* refused:
  "A wire needs two different places".
- `save_document as: "x.txt"` for a schematic writes it as `.txt`, which then opens as text, while
  `as: "noext"` gets `.sch`. *Fixed:* refused: a schematic's file ends in .sch, a data display's in
  .dpl, a symbol's in .sym.
- An untitled text document and an untitled schematic are both "untitled" (closeUntouched closes
  schematics alone). *Fixed:* the tools name the second "untitled (2)", and so on, so each can be
  named.
- With one ground, get_schematic's `components: ["GND#1"]` finds nothing, while move and select take
  `GND#1` (`one.json`). *Fixed:* `GND#1` is taken for the one ground.
- tune's table shows `1.000000k Ohm` for `1k`. *Fixed:* values without the zeros after the point
  (1k).
- `rename_net from: gnd`: "No net is called gnd (get_schematic lists the nets)", but get_schematic
  calls that net `gnd` (`rengnd.json`). *Fixed:* "gnd is ground: it keeps its name".
- `open_document` of random bytes, or of a schematic with an unknown component type: "could not be
  opened", without the reason (`NoSuchType` is unknown). *Found right in this build:* the reason
  follows ("Qucs-S reported: Wrong document type", "Unknown component: NoSuchType").
- A ground made inactive (`edit_component GND active: false`) still makes its net `gnd` to
  get_schematic, while check_schematic says "no ground" and the netlist has `_net1`: `netsOf`
  takes a GND by its type, active or not (`inact.json`). *Fixed:* `netsOf` grounds a net only by an
  active ground.
- `diff against: /bin/ls` reads the program as an empty schematic, so every part is "added",
  rather than saying it is no schematic (`diffbin.json`). *Fixed:* refused: "... is not a schematic
  of Qucs-S".

## G. Checked and found right

- **Every component type**: describe_component_type and a preview of add_component for all 178
  types, under ngspice, Xyce and Qucsator, on the ASan build: no crash or UB (`alltypes.py`).
- **Fuzzing**: 9,650 random calls one at a time and 11,600 in pairs, over all 61 tools in reach, on the
  ASan/UBSan build: no crash or hang, and one UB report (E6). Bad arguments are refused with a
  message.
- **JSON-RPC**: malformed JSON, odd ids, CRLF, blank lines, a 50 MB request, `notifications/cancelled`
  and unknown resources are all answered; the server keeps running.
- **Pictures**: `export_image` at scale 20 of a schematic 8.4 million units wide is capped at
  10,000 × 10,000 pixels (1.9 s). The formula painting with 20,000 nested braces and 5,000
  `\sqrt` did not crash.
- **Axes**: step 0, a log axis through 0, and from ≥ to are refused. A 1e-300 step over 1e300
  draws in 0.04 s.
- **get_dataset**: THD with a 1e-300 Hz or 1e300 Hz fundamental, `fft` or `distribution` on one
  point, and `at` 1e308 are all answered.
- **copy_document** refuses its own file under every spelling (`self`, `SELF.sch`, `./self.sch`,
  the folder). new_project refuses `../` and `/`.
- **Speed**: reads of 15,000 parts take 0.06-0.24 s (get_schematic, check_schematic).
  import_netlist of 2,000 elements takes 0.28 s. list_documents of `/` takes 2 s.
- **Today's features** worked as meant in their own tests: refs in move, delete, select and
  edit_component; the untitled simulate (including in a project's Scratch); the subcircuit with
  its pins; the preview putting its file back; expressions in `plot` traced with data.

## Reproducing

Beside this report, in `2026-09-28-claude-tools/`:
- `mcp.py` runs a list of calls, `[[tool, args], ...]`, one after another against
  `qucs-s --mcp-server`. `_sh` and `_sleep` entries run a command or wait. `QUCS` picks the
  binary, `HUNT_SETTINGS` a settings folder and `HUNT_WORKSPACE` the workspace (`ws/` beside it by
  default).
- `pipe.py` sends each group of calls at once, without waiting between them (A2, A3).
- `fuzz_tools.py SEED CALLS` is the fuzzer; `rpc.py` the JSON-RPC probe; `roundtrip.py SEED N` the
  JSON round trip (A4). `RT_EX` points it at a copy of the examples, `RT_SUB=/` takes all of them,
  and `RT_BESIDE=1` saves each copy beside its original, so its subcircuits and libraries are found.
- The fuzzers read the server's answers through a buffer of their own. `select()` on Python's
  buffered pipe waited for answers already read (two coming back together), which a first run
  after the fixes showed as hangs of the server; they were the client's.
- The `*.json` files are the cases named above. `HUNT` in them stands for the folder whose `p/ws`
  holds the schematics (tn.sch, rc.sch: built by the calls at the top of `race3.json` and `mkrc.json`).

Settings must be isolated, and the workspace set to a scratch folder. Otherwise tools write into the
real workspace: a run of `fuzz_tools.py` at 20:28 put 105 files and projects (`"`, `{}`,
`<Components>_prj`, ...) into `~/QucsWorkspace`, because its settings folder was empty and the
workspace fell back to the default (found in the fourth round of feedback,
`docs/feature_gaps/2026-09-28-claude-mcp-api-feedback-round4.md`). The scripts now start the server
with `--workspace` (an option added in that round), and each fuzzer seed has its own folder,
`runs/<seed>/w/ws`, two folders down so that a call given `..` or `../..` stays inside `runs/`. The
fuzzers and `mcp.py` stop before the first call if `get_state` names another workspace. The fuzzers
also set `HOME` to `runs/<seed>/home`, because a folder opened as a project keeps its scratch files in
`~/Library/Caches/qucs-s/projects`. What the scripts leave beside them is ignored (`.gitignore`).
