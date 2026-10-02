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

### A2. CI's "Claude's tools take what they read" fails

`scripts/ci/check-tool-arguments.py` reports `get_dataset: no handler found in call()`:
the dispatch became a block (`getDataset(args)`, then `misc::releaseFreedMemory()`), and
the checker looks for `return getDataset(args);`. CI's Linux job stops at this step on
`6b2df9f`. It passed this step on `d19221d`.

*Fix:* release the memory inside `getDataset` (or a wrapper the checker knows), keeping
the dispatch a single `return`.

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

### B2. `get_state` names a selected ground `""`

`qucscontrol.cpp:4482` lists the selected parts by `c->Name`: a selected ground is `""`,
not its ref `GND#2` that `select`, `move` and `delete` take. This is the naming of the
stress report's bug 2, in a place its fix did not reach (`p13`, `p34`).

*Fix:* `PartIndex::refs(sch)` there too.

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

### B5. Open Project chosen on the open project

`slotProjectsContextMenu` compares the row's path with the open project's by
`QDir::cleanPath`, not by the folder. With the workspace given as `/tmp/...` (a link to
`/private/tmp`) and the project opened by its real path, the open project's own row offers
Open Project (`p25`). Choosing it closes every document (asking about unsaved changes)
and opens the same project again. The same holds for a workspace path through any link,
and for case on macOS.

*Fix:* compare canonical paths (`QFileInfo::canonicalFilePath`), as `openProject` does
for the workspace and home folders.

### B6. Two parts of one name in a selection

A hand-edited file with two parts named R1 (Check Schematic says "R1: the name is used
twice"): Select All, then `move {selection: true}` moves R1 (1k) and the ground, and
leaves the other R1 (2k) where it was; `delete {selection: true}` deletes the first R1
only. Both answers list "R1" as done (`p33`). `withSelection` turns the selection into
names, and both resolve to the first. This was so before the stress fix, which kept it.

*Fix:* pass the selected parts themselves (or their index in `a_DocComps`) from
`withSelection` to the handlers, not their names.

### B7. A PAM4 source's trace measured as NRZ

The eye takes the unit interval from the V(PRBS) source ("unit interval from: V1's
Tbit"), but not the levels from its Coding. The eye diagram (levels 2 by default) and
`get_dataset` without `levels` measure a PAM4 PRBS7 through an RC as one NRZ eye
(levels 0.16 and 0.83, Q 2.03), saying nothing of PAM4 (`p35`). With `levels: 4` the
same data gives its four levels and three eyes.

*Fix:* with no levels given, take 4 from a source coded PAM4 (and say so, as for the
Tbit).

### B8. One eye diagram over two PRBS sources

Traces from V1 (Tbit 100 ps) and V2 (200 ps) in one eye diagram are both folded at 100
ps. Trace 2's measurement says "unit interval from: V1's Tbit" (`p39`). `get_dataset` on
the same two variables gives each its own source's Tbit, so the two disagree.

*Fix:* each trace its own unit interval (the diagram draws one grid in UI: fold each
trace at its own), or say that trace 2's source has another Tbit.

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
without HOME, and `QUCS_CACHE_DIR` was not set for that run. They were left there.

The system's Trash changed at 21:05, during the hunt. It cannot be listed, every run had
its own trash (`QUCS_TRASH_DIR`), and the user's own Qucs-S was running then.
`~/QucsWorkspace` changed only by that Qucs-S (`opamp741_prj`).

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
