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

*Status:* none fixed yet.

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

Add R1, add R2, undo, then any tool with `preview: true`. The redo step is gone ("There is nothing
to redo"), although the preview says nothing is changed. `forgetUndoAfter(marks)` cuts at the
undo position, not the stack's end. A `run_script` with `atomic` that throws does the same, even when
it changed nothing. The atomic batch checks the revision first and keeps it. `redo.json`, `redo2.json`.

### A6. import_netlist changes the circuit: node names split by case, `n+1` merged with `n_1`, the first element eaten as the title

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

`simulate` with `keep_as: "rc"` writes `rc.dat.ngspice` beside the schematic. If `rc.sch` is there,
its dataset is replaced by this run's without a word (md5 changed; `keep.json`). Its diagrams then
show another circuit's results. *Fix:* refuse a name that is a schematic's there, or prefix kept
runs (`name.run.dat...`).

### A8. Two imports of one title share `<title>_subcircuits.lib`: the second changes the first's subcircuits

Without `save_as`, the subcircuits of an import go into `<title>_subcircuits.lib` in the workspace.
A second netlist of the same title overwrites it. The first schematic, still open, includes that
file by path, so its `X1` silently becomes the second netlist's `blk`, an R turned into a C
(`lib2.json`). *Fix:* a unique name (as with `save_as`), or refuse to write over one.

### A9. A batch's calls without a path follow "the document in front": when that changes mid-batch, the rest land in another document

A batch runs its calls one per event-loop turn. Each call without `path` acts on the document in
front *when it runs*. A `close_document` sent behind a batch of three `add_component` closed the
untitled schematic after the first. The other two, CB2 and CB3, were added to plain.sch, the
document then in front. Every call answered as done (`close.json`, group 5). In the window, the
user switching tabs during a batch does the same. *Fix:* resolve the batch's document once, when
it starts, for the calls that give no path.

### A10. A Save As that fails leaves the document named after the file it could not write: every save after fails too

`QucsApp::saveDocumentAs` gives the document its new name (`Doc->setName(s)`) before it saves,
and does not give the old one back when the save fails. `save_document as: /System/nope.sch` on
tn.sch with an edit answers "could not be saved". The document is now `nope.sch`, at
`/System/nope.sch`, unsaved. A plain `save_document` then fails again, and closing asks to save or
discard: the edit can reach tn.sch only by another Save As (`failsave.json`). The window's Save As
shares the function. *Fix:* keep the old name, file info and tab text when `save()` fails.

### A11. One file opens as several documents - through a symlink, or in another case on a case-insensitive disk

`open_document` of `ws/tn.sch`, then of `wslink/tn.sch` (a symlink to `ws`), then of
`ws/TN.sch` (APFS ignores case) gives three documents of one file (`twice.json`; `../` is caught).
Each keeps its own edits, and the last one saved writes over the others. get_state lists
`tn.sch` twice, so the bare name is ambiguous too. `openDocument` compares with `sameFile`,
but `gotoPage` finds an open document by its exact name, and opens a new one. *Fix:* find open
documents by canonical path (as `reloadChangedFiles` already does).

### A12. A file it may write but not read is deleted when the tool fails or is previewed

Today's put-back keeps what the file held by reading it first: `if (old.exists() && old.open(ReadOnly))`.
A file that can be written but not read (mode 200) reads as absent. The tool then writes it
(`replace`), and on failure, or at the end of a preview, puts back "no file", which removes it.
A preview of `create_subcircuit save_as: wosub.sch replace: true` over such a file answered "nothing
changed, and wosub.sch is as it was", and the file was gone (`wo.json`). Its subcircuit also had 0
pins, since the file written could not be read back. *Fix:* refuse to replace a file that cannot be
read, or keep "unreadable" apart from "absent".

## B. Crashes and hangs

### B1. A deeply nested expression crashes Qucs-S (stack overflow in the dataset parser)

`get_dataset` evaluates expressions with a recursive-descent parser (`dataset.cpp`, `Parser`).
`"-" * 20000 + "v(out)"`, or 100,000 parentheses, overflow the stack. The ASan build reports
`AsanOnDeadlySignal` in `Parser::skip`/`take` (crash report `qucs-s-2026-09-28-141623.ips`); the
Release build dies as well. One tool call takes the whole window down, and any unsaved work with
it. `deep2.json`. *Fix:* a depth limit in the parser (refuse beyond, say, 200 levels).

### B2. `eye` with a tiny `bit_period` never ends and eats all memory

`get_dataset {measure: ["eye"], bit_period: 1e-300}` loops at `dataset.cpp:1426`,
`for (t = start + centre*T; t <= c.x.last(); t += T)`. Once `t + T == t`, it never advances, and
`highs`/`lows` grow for good. It reached 13 GB RSS after 10 minutes and was killed (`meas.json`,
call 2). `thd` with extreme fundamentals is refused properly. *Fix:* refuse a `bit_period` that
gives more bits than samples (or more than, say, 1e6).

### B3. arrange is quadratic: a minute of frozen window at 3,000 parts

`arrange` (preview) of a grid of resistors in chains took 0.54 s at 300 parts, 5.7 s at 1,000
and 59.5 s at 3,000. It succeeds, but the window is frozen for a minute and says nothing
(`big300.json`, `big1000.json`; the 3,000-part case is the same grid, 60 to a row). By reading: each wire's way is checked with a full `netsOf()` of the schematic.

### B4. Opening a read-only file, or one of a newer version, hangs the MCP server for good (a modal box)

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

`rename_net out -> 0` is accepted, and the netlist reads `R2 0 0 1K`: the net is ground now.
`-> GND` is accepted too, and ngspice takes `gnd` as ground. `set_label 0` on a source's pin gives
`V1 0 0 DC 1`. check_schematic says nothing. rename_net refuses a name another net has, but does
not count ground's names. `gndname.json`.

### C2. Net and part names are not checked as the GUI checks them: spaces, `;`, `$` break the netlist

The label dialog allows `[a-zA-Z]([0-9a-zA-Z]|_(?!_))+`; the tools take anything.

- `set_label "a 0"` gives `R1 _net0 a 0 1K`, an extra node.
- `x;y` and `n$1` begin ngspice comments. `a=b`, `{a}` and `v(x)` are read as other things.
- `edit_component rename`: `R 9` gives `R 9 _net0 0 1K`. `R;9` and `R$9` begin comments.
  `R9.2` makes its pins `R9.2.1`, which connect cannot tell. `9R` is netlisted as `R9R`.
  `add_component` names the same.

`labels.json`, `names.json`. *Fix:* the dialog's rule for labels, and SPICE-safe part names, in
the tools.

### C3. A net renamed to `net2` while another net is `net2`: two nets of one name

`rename_net net1 -> net2` is accepted while get_schematic calls another net `net2`. The next read
lists two different nets named `net2`, and `rename_net from: net2` takes the labelled one. The
automatic names also shift after any label, so a name read before now means another net
(`netname.json`). *Fix:* refuse `net<n>` names, or number automatic nets so they cannot collide
(`_n1`).

### C4. Grounds alone made into a subcircuit: the circuit loses its ground, silently

With ground refs, `create_subcircuit names: ["GND#1", "GND#2"]` is taken. The two ground symbols
move into `grounds_only.sch`, a subcircuit with no ports ("grounds inside": 2), and `SUB1` takes
their place. The top schematic has no ground symbol left, so V1 and R2, which stood on them,
float (`cs.json`, overview: no GND). *Fix:* refuse a group that carries no part with a pin
other than a ground, or leave the grounds behind.

## D. Today's changes (`e6df622`)

### D1. The typo check refuses real units, and blocks unrelated `.sch` text edits

The new check refuses a number followed by letters that are not a scale and a unit. Its unit list
is too narrow: `1 kOhms`, `5 Volts`, `2 Amps`, `1 Hertz`, `10 dBV`, `3 dBc`, `25 degC` and
`1.5 V/us` are all refused (`typo.json`). In set_schematic's text form, the check runs over *every*
component after the text is read, not only the lines given. On a schematic that holds `1 kOhms`
(`legacy.json`), a text of `<Paintings>` alone is refused. *Fix:* plurals and spelled-out units,
units with a slash and dB variants; check only the components in the text given.

### D2. A file dated in the future is loaded again just after it is opened, and told as changed on disk

The watch now looks at a file when its watch is set, and loads it again if the file is newer than
its last load. A file whose date is in the future (clock skew, a file unpacked from an archive)
is always newer. Opened with `open_document`, it was loaded again ~1.5 s later. Claude was told
"future.sch was changed by its file, changed on disk ... What changed: DC1: its text moved"
(`fut.json`). That the second load differs from the first is a small oddity of its own. *Fix:*
compare the file's date with the date it had when loaded, not with the time of the load.

### D3. Expressions in a diagram's traces are still dead traces

Today's fix for `plot` covers add_analysis alone. `add_diagram` and `add_trace` still take
`ac.db(v(out))`, `ac.v(out)/2` and `ac.mag(v(out))`. They say "no variable ... until a simulation
writes it", and after a successful run they are still listed under "traces without data"
(`trace.json`): the dead end the feedback reported. *Fix:* the same NutmegEq, or refuse with the
way to do it.

### D4. Expressions in `plot` are written into the NutmegEq unchecked

`plot: ["x=1", "v(out) + "]` gives equations `x_1=x=1` and `v_out_2=v(out) +`. They fail only at
the next simulation, far from the call that made them (`typo.json`, last call). `2e` is also
taken as a number (E, exa).

### D5. The schema has no `tex` (a formula's text), and today's description of `text` says it is

A formula painting takes `tex` (and `display`), which add_painting's schema does not list.
Today's field description of `text` says "a formula's text (TeX for a formula)". Following it
gives "A formula has no text; it takes: angle, color, display, size, tex, x, y."

## E. Wrong behaviour

### E1. No analysis block: "Failed to start simulator: Unknown error"

`simulate` of a schematic with no analysis, saved or untitled, answers "The simulation did not start.
Failed to start simulator \"/Users/meisam/bin/ngspice\": Unknown error". The reason, "no simulation:
no .AC, .TR, .DC ... block", comes only among the Check Schematic warnings after it (`noan.json`).

### E2. A sign-in failure shows Claude Code's text alone; `/login` cannot run in the dock

A turn that fails shows `failureOf(subtype, text)`: Claude Code's own message, "Failed to
authenticate" or "Please run /login". `/login` does not work in the dock (a print-mode session),
and nothing says to sign in with `claude` in a terminal. A user asked what to do on 28 September
(by reading `claudecodepanel.cpp` `onTurnFinished`, `claudecode.cpp` `handleResult`).

### E3. `at` past the curve is put on its last sample; `label_offset` overflows

`add_marker at: 1e308` on a 1 ms transient puts the marker on the last sample, time 0.001, and
says nothing. `label_offset [2^31-1, 2^31-1]` is accepted: the label lands at
`[-16777076, 16777896]` (x's sign flipped), and the diagram's screenshot becomes 10,001 × 9,999
pixels (`mk.json`).

### E4. A simulation whose document is closed while it runs answers with another run's log, or with no reason

`simulate` of rc.sch, with `close_document` sent right behind it, once answered with a log of
plain.sch, headed "qucs 26.1.3" (a stale log), with no dataset. Another time it answered "The
simulation did not start." with no reason (`close.json`, `close2.json`). The answer should say the
document was closed.

### E5. A library part cannot be placed: "There is no component type Lib", though find_library_component says to

find_library_component's answer says a Qucs library part is placed with `add_component` of type
`Lib`, with its `Lib` and `Comp`. add_component, describe_component_type and set_schematic's JSON
form all answer "There is no component type Lib". `newComponent()` looks the type up in the
module registry, where `Lib` is not: `getComponentFromName()` makes a `LibComp` for it by hand
(`lib3.json`; also seen in A4's round trip). A library part can be placed only through
set_schematic's `.sch` text. *Fix:* make `Lib` in `newComponent()` as the loader does (and check the loader's other
hand-made types the same way).

### E6. `diff steps: -2147483648`: signed overflow (UBSan) before the range check

`diffTool` computes `const int at = sch->undoIndex() - steps;` and checks `steps < 1` only after it.
Given INT_MIN, it overflows: UBSan's `qucscontrol_resources.cpp:367:41: runtime error: signed
integer overflow: 2 - -2147483648`. Found by the pairs fuzzer (`fuzz_pipe.py`, seeds 43, 44 and 54).
The answer is still the refusal. *Fix:* check `steps` first.

### E7. A dataset that cannot be written: no error in the answer - the reason is in a modal box

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
  `~/Library/Caches/qucs-s/projects/ws-…`).
- `set_label` with `at: null`: " is not a pin ..." (the empty name leads the message).
- JSON-RPC: a request array is refused as "Parse error: no error occurred"; `id: null` is answered
  with id 0; `params: [1, 2]` gives "There is no tool ." rather than -32602 (`rpc.py`).
- `move_to_pane pane: 1.5`: "There is no pane 0".
- `screenshot region [-2^31, -2^31, 2^31-1, 2^31-1]`: "nothing ... at the region", although the parts
  are inside (the rectangle's width overflows).
- `add_wire [[0,0],[0,0]]`: "Wire drawn through 2 places", and nothing is drawn.
- `save_document as: "x.txt"` for a schematic writes it as `.txt`, which then opens as text, while
  `as: "noext"` gets `.sch`.
- An untitled text document and an untitled schematic are both "untitled" (closeUntouched closes
  schematics alone).
- With one ground, get_schematic's `components: ["GND#1"]` finds nothing, while move and select take
  `GND#1` (`one.json`).
- tune's table shows `1.000000k Ohm` for `1k`.
- `rename_net from: gnd`: "No net is called gnd (get_schematic lists the nets)", but get_schematic
  calls that net `gnd` (`rengnd.json`).
- `open_document` of random bytes, or of a schematic with an unknown component type: "could not be
  opened", without the reason (`NoSuchType` is unknown).
- A ground made inactive (`edit_component GND active: false`) still makes its net `gnd` to
  get_schematic, while check_schematic says "no ground" and the netlist has `_net1`: `netsOf`
  takes a GND by its type, active or not (`inact.json`).
- `diff against: /bin/ls` reads the program as an empty schematic, so every part is "added",
  rather than saying it is no schematic (`diffbin.json`).

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
  binary and `HUNT_SETTINGS` a settings folder.
- `pipe.py` sends each group of calls at once, without waiting between them (A2, A3).
- `fuzz_tools.py SEED CALLS` is the fuzzer; `rpc.py` the JSON-RPC probe; `roundtrip.py SEED N` the
  JSON round trip (A4).
- The `*.json` files are the cases named above. `HUNT` in them stands for the folder whose `p/ws`
  holds the schematics (tn.sch, rc.sch: built by the calls at the top of `race3.json` and `mkrc.json`).

Settings must be isolated, and the workspace set to a scratch folder
(`QucsHomeDir`, `location`, `S4Q_workdir` in `qucs/qucs_s.ini`). Otherwise tools write into the real
workspace. Even then the untitled simulate writes into `~/Library/Caches/qucs-s/projects` unless
the workspace has a `Scratch` folder.
