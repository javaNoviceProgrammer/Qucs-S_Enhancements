# Bug hunt: data in and out, the wishlist, and the nets behind them

*30 September 2026 - Qucs-S 26.1.4, `6511b15`.*

One hour, 19:51 to 20:51, in the foreground. It covered what came after the last hunt (the tools
and Check Schematic at `a643ac8`):
- the Import tab and `import_data`;
- the Export tab and `dataexport`;
- the rules for naming a trace of another dataset (`name:variable`);
- the fix of nets named as ground (`d62605f`);
- the wishlist of 30 September: subcircuit parameters, `ACmag`, the two new rules of the check,
  the kept netlists, Verilog-A outside a project, dialog trees, data displays;
- rounds 10 and 11: previews, scratch folders, `clean_scratch`, `max_chars`.

The Release build of `6511b15` drove everything. Each `qucs-s --mcp-server` ran with its own
workspace, settings and HOME in a scratch folder, on macOS arm64 with ngspice 46. The ASan build
was of the morning (07:53, before the wishlist), and rebuilding it would have taken a good part
of the hour. So the fuzzers ran on the Release build, where a crash still shows as a server that
dies, but a sanitizer's report would not. Xyce, SPICE OPUS and Qucsator are not installed here:
what is said of them comes from their netlists, not runs. Methods:

1. **Probes**: the tools driven as an agent drives them, one question at a time (scripts beside
   this report). Where a netlist was in doubt, it was simulated.
2. **Sweeps**: the two new rules of the check over all 251 examples, and the end-to-end
   scenarios s1 to s10: 77 checks, none failed.
3. **Fuzzers**:
   - 30,502 random calls with junk arguments to 12 tools (`import_data`,
     `set_subcircuit_parameters`, `make_symbol`, `add_diagram`, the trace tools, `get_dataset`,
     `list_documents`, `clean_scratch`, `copy_document`, ...);
   - 400 random datasets exported in every format and form, 25,542 exports, each read back
     (a test slot kept beside this report as a patch, not in the tree);
   - 60 rounds of `set_dialog` on Find and Replace's result tree;
   - the Export tab under all 12 diagram kinds, each format and form, with the diagram's traces
     (one of them in no dataset, one an expression): 108 exports, the graphs kept on OK
     (`h_kinds.patch`).

   No crash and no server that died, but for the one probe of F1. Nothing was written outside the scratch folder. Two probes
   (A7) wrote outside their own workspace on purpose, still inside the scratch folder.

*Status:* found during the hunt, not fixed then (as asked: the hunt went on past each finding). All 30 were fixed afterwards: F1, A1, A6, A8, A9, D1 and D4 in `a1f731a`; A2-A5, A7, D2 and D3 in `155f3d2`; B1-B5 in `003aac6`; C1-C9 in `5c89ca0`; D5 and E1 in `8f88220`. Each fix has a test (but D1, whose part is not built in this tree, and D4, a test's own fix), and each test was checked by breaking the fix it covers: 81 breaks, each caught (five after a test was made sharper) but one that changes nothing a reader sees (B4: a CR LF read as two line ends, and the empty line skipped). E1 is tested by reading and refusing only: no test runs a command.

`~/QucsWorkspace` and `~/Library/Caches/qucs-s` were not written by these runs. During the hour,
something else wrote `~/QucsWorkspace/pi_fit_S11_mag.csv` (19:53). No probe names it, and every
probe had a workspace of its own. The probes of `remove` and `clean_scratch` sent their own
scratch files to the system's trash, as `clean_scratch`'s test does.

| | severity | area | finding |
|---|---|---|---|
| F1 | high | crash | A subcircuit with two ports of the same number crashes Qucs-S when a schematic that uses it is netlisted |
| E1 | high | security | Claude's `simulate` runs a schematic's commands, unannounced: a System command part (`CMD`) after the run, ngspice's `shell` in a custom simulation. Nothing asks about them or reports them |
| A1 | high | netlist | A net with two names - a label and a ground, or two labels - is netlisted as two nodes: parts on one name are cut off from the others. Check Schematic says nothing, or that "the netlist keeps one of them" |
| A2 | medium | `clean_scratch` | `datasets` trashes `<file name>.dat*`, not the schematic's Data Set: another dataset goes (an import), its own stays |
| A3 | medium-low | `copy_document` | A schematic whose Data Set is not its file name is copied without its dataset |
| A4 | medium-low | data display | A data display reads the dataset of its own name, not its schematic's Data Set: "simulate to make it" after a simulation |
| A5 | medium-low | `simulate` | `keep_as` takes an imported dataset's name: the kept run cannot be named in a trace, nor chosen in the Data tab |
| A6 | low-medium | netlist / check | A net named GND (not gnd) is ground to the check for every SPICE simulator, and to ngspice; SPICE OPUS (and Xyce) read an ordinary node |
| A7 | medium-low | Data Set, Data Display | A schematic's Data Set may be a path out of its folder (`../up.dat`): its run writes there. A Data Display that cannot be opened as one still leaves an empty file, out of the folder too |
| A8 | medium | netlist (subcircuits) | A subcircuit with two ports on one net (a pass-through) is written `.SUBCKT thru P1 P1 P3`: ngspice connects one of them, silently, and the other port is open |
| A9 | high | netlist (subcircuits) | A port's net named after it can be another net to ngspice: a port named gnd or GND makes its pin ground inside (`.SUBCKT sub P1 gnd`), and a port P1 beside a net labelled p1 shorts the two. A regression of `9d583cf` |
| B1 | medium | Export tab | Memory and time: 2.5 million values to CSV peak at 809 MB (a 17 MB file); a workbook of 500,000 rows takes 1.6 GB and 5.3 s, on the GUI thread |
| B2 | low-medium | Export tab | The file is written in the format chosen whatever its name: `out.xlsx` with CSV chosen holds CSV text |
| B3 | low | Export tab | A dataset written beside the schematic is listed in the Export tab, not in the Data tab |
| B4 | medium-low | import | A CSV of 26 MB (500,000 rows of 5 columns) took 2.3 s to import, and the server held 1.15 GB after: it is read as spreadsheet cells too |
| B5 | medium-low | Export tab | The file it proposes for an imported dataset is the file it was imported from (`bench.csv` for `bench`): one Replace away from losing the measurement |
| C1 | medium | tools | A trace with no data reads every imported dataset beside it in full, for a hint: `add_diagram` of 8 such traces 9.8 s, `get_schematic` 4.9 s |
| C2 | medium-low | `run_script` | `qucs.call` refuses `max_chars`, which a direct call and `batch` take |
| C3 | low-medium | `get_dataset` | It gives an imported variable's trace as `m:gain`, and refuses `m:gain` as a variable |
| C4 | low | `import_data` | A schematic (`.sch`) is read as text of numbers: its coordinates become five columns |
| C5 | low | `set_subcircuit_parameters` | Any default without a space is taken: `-`, `1k;`, `1,5` |
| C6 | low | `make_symbol` | A prefix with `;` is taken: the instance X;Y1 is a comment to ngspice from the `;` |
| C7 | low | `set_dialog` | A tree's row out of range (or a tree of no rows) is answered "does not take [99,0,true]", not why |
| C8 | low-medium | `import_data` | Every column of a table is listed: 5,000 columns made an answer of 112 KB, not cut (the variables and traces are cut at 100) |
| C9 | medium-low | `get_dataset` | A kept run's variables are given traces of the current run: `ngspice/ac.v(in)` for `run1.dat.ngspice`, not `ngspice/run1:ac.v(in)` |
| D1 | low | Check Schematic | Vac_SPICE counts as an AC source whatever its text; one with no AC value drives nothing, unsaid |
| D2 | low | cache | The netlists kept for the datasets are never removed |
| D3 | low | `clean_scratch` | In the folder schematics of no project share, it trashes every file there, not only its last run's |
| D4 | low | test | `aNetNamedAsGroundIsNotPrinted`'s cases "with a ground symbol" put it mid-wire, connected to nothing |
| D5 | low | SPICE file (upstream) | The Preprocessor option starts the wrong program: the script's name, with `perl` as its first argument |

## F. A crash

### F1. Two ports of the same number crash the netlister

A subcircuit of three ports, with P2's number set to 1 as P1's is (`edit_component`, as its
dialog does): Check Schematic said nothing of it, nor did the parent's check with `subcircuits`
("in its subcircuits 0 errors", `h_portnum_check.py`). An instance of it placed in another schematic
showed three pins. `get_netlist` of that schematic killed the server (`h_portnum.py`). The crash
report (its crashed thread in `crash-portnum.txt` beside this report) gives a bus error in `QString::operator=` (a
pointer that fails authentication) under `Schematic::throughAllComps`, from `giveNodeNames`,
`prepareSpiceNetlist`, `SaveNetlist`, and `QucsControl::getNetlist`. By reading, it is the loop
that gives each pin of an instance its port's type: `pp->Type = it.value().PortTypes[i]` for
every pin. The subcircuit's file gives one type for each port number, so two ports of one number
give fewer types than the instance has pins, and the index runs past the list (no check in a
Release build). The window crashes the same way on Simulate or on any netlist of the parent.

*Fix:* Check Schematic calls two ports of one number an error, and so does the netlister. The
loop stops at the shorter of the two lists.

**Fixed in `a1f731a`.** `Schematic::throughAllComps` stops at the ports the subcircuit has, in both of its loops, so no index runs past `PortTypes`. Check Schematic says why: "P1 and P2 are all port 2: each port needs a number of its own, or an instance's pins and the subcircuit's do not match". Test: `test_symbol_pins` `twoPortsOfOneNumber`.

## A. Wrong answers, or silent

### A1. A net with two names is netlisted as two nodes

The netlister names each net from what is on it: a ground symbol's node is `gnd` (0), a label's
wire takes the label's name. When one net has two of these, they do not meet: each part's pin
gets whichever name reached it, and the netlist has two nodes where the schematic has one net.
Check Schematic (`netsOf`) joins them as one net, as the schematic shows. So it says nothing of
the ground case, and of two labels it says (a note, "fine if intended"): "one net has 2 names, a
and b: the netlist keeps one of them, and a plot or an equation of another finds nothing - keep
one label". The netlist keeps both.

Three ways in (where ngspice was run on them, it "succeeded"):
- **A labelled net wired to a grounded one** (`h_merge.py`): V1, R1 on ground; R2 on a net
  labelled `ret`; then R1.2 connected to V1.2 (one wire, as in the GUI). The netlist:
  `V1 _net0 0`, `R1 _net0 0`, `R2 _net0 ret`. R2's second pin is on a node nothing else is on.
- **A label, then a ground** (`h_split2.py`): `set_label foo` on R1.2's net, then `connect R2.2`
  to `ground`. Every pin of the net is on `foo` and nothing is on 0: the circuit has no ground at
  all.
- **Two labels joined** (`h_merge2.py`): nets labelled `a` and `b`, then wired together: R1 and
  R2 on `a`, R3 on `b`. `get_schematic` lists one net with R1.2, R2.2 and R3.2; `get_netlist`'s
  `map` gives `"a": ["R1.2", "R2.2"], "b": ["R3.2"]` (`h_map2.py`).

In the GUI, a ground placed on a labelled wire drops that wire's label
(`Schematic::insertComponent`), and a label cannot be placed on a grounded wire ("The ground
potential cannot be labeled!"). Neither covers a ground placed apart and then wired to a
labelled net, or two labelled nets wired together. `set_label` refuses a grounded net, but
`connect` joins any two. A file can hold either too (`h_split.py`: a ground at the end of a
labelled wire, `V1 out foo`, `R1 0 out`). ngspice hides the ground case only when the label is
gnd in any case (its own name for 0); SPICE OPUS and Xyce see the nodes as written.

The naming is upstream's: `Schematic::giveNodeNames` sends each named node's name along its net
(`propagateNode`) until it meets nodes already named, unchanged since the source was vendored
(`862341e`). So two names on one net stay two. The check's note claiming one of them is kept is
ours, and so is the silence about a label on a grounded net. `arrange` is consistent with the
netlist here: on the net with two labels it refuses ("the net label b would be gone"), and with
`keep_places` it keeps both (`h_arrange2.py`).

*Fix:* when names meet on one net, the netlister should name it once, with ground first and then
one label, as `netsOf` does, and Check Schematic should say which label is dropped. At least the
check should tell a net that is ground and labelled, and say what the netlist does with two
labels.

**Fixed in `a1f731a`.** `Schematic::unifyNamedNets` joins the nets as the check's `netsOf` does, after the ground and the labels name them: each net is one node, named gnd when it is ground, else by the first of its names in order. A label's name that the netlist drops is not printed. The check's note says which name the netlist keeps ("one net has 2 names, a and b: the netlist calls it a, and a plot or an equation of b finds nothing - keep one label"), and a label on a grounded net is a note of its own. Test: `test_erc` `aNetWithTwoNamesIsOneNode`.

### A2. `clean_scratch` with `datasets` trashes by the file's name

`amp.sch` has its Data Set set to `run.dat`. Beside it is `amp.dat`, a dataset of that name (an
import, say). After a simulation, `clean_scratch` with `datasets: true` answered "Cleared: ...;
amp.dat". The folder held `amp.sch`, `run.dat.ngspice` afterwards. It took a dataset that is
not the schematic's and left its own (`h_clean.py`). It looks for
`completeBaseName() + .dat, .dat.ngspice, ...` (`qucscontrol_design.cpp`, `cleanScratch`).

*Fix:* take the datasets from `getDataSet()`. A `.dat` with an import's origin is never a
simulation's.

**Fixed in `155f3d2`.** `clean_scratch` with `datasets` trashes the datasets of the schematic's Data Set (each simulator's), and the netlists kept for them; a `.dat` with an import's origin is never one. Test: `test_qucs_control` `theDataSetNamesTheResults`.

### A3. `copy_document` leaves a dataset behind when the Data Set is not the file's name

The examples' templates keep their own Data Set: `AC_Passive_analysis.sch`, copied as `a.sch`, has
`<DataSet=AC_Passive_analysis.dat>`. After a simulation, `copy_document` to `p q` wrote only
`p q.sch` (`"written": ["p q.sch"]`), not the dataset (`h_names.py`). Its
description says it copies "a schematic together with its datasets and data display". It finds
them by the file's name, as A2 does.

*Fix:* the same: by `getDataSet()` (and `getDataDisplay()`).

**Fixed in `155f3d2`.** `copy_document` copies the Data Set's datasets and the Data Display's `.dpl` (from the open document, or its file's `<DataSet=...>`), not an import of the file's name. Test: `theDataSetNamesTheResults`.

### A4. A data display reads the dataset of its own name

`add_diagram` with `document: "data_display"` on `amp.sch` (Data Set `run.dat`, Data Display
`amp.dpl`) made `amp.dpl` with `<DataSet=amp.dat>`. Its trace read `amp.dat.ngspice`: "there is
no dataset amp.dat.ngspice (simulate to make it)", right after a simulation wrote
`run.dat.ngspice` (`h_dpl.py`). The same diagram on the schematic had 59 points. A new
`Schematic` takes its dataset from its own file name. The GUI's Change to Data Display (F4)
makes a `.dpl` the same way (upstream's behaviour), and the wishlist's `document:
"data_display"` inherits it.

*Fix:* a data display made for a schematic takes its Data Set.

**Fixed in `155f3d2`.** A data display made for a schematic (F4, `add_diagram` with `document: "data_display"`, `new_document`) takes the schematic's Data Set. Test: `theDataSetNamesTheResults`.

### A5. `keep_as` takes an imported dataset's name

`run1.csv` was imported as `run1`. `simulate` with `keep_as: "run1"` then wrote
`run1.dat.ngspice` beside the import, and said so. But `run1:ac.v(out)` now means the import (an
imported dataset's traces have no prefix): "run1.dat has no variable ac.v(out); its variables:
g". Only `ngspice/run1:ac.v(out)`, given in full, reaches the kept run. The diagram dialog's Data
tab lists `run1` once, as imported, and offers no simulator for it, so the kept run cannot be
chosen there (`h_keepas.py`). `import_data` refuses a simulation's dataset's name; `keep_as` does
not refuse an import's. By reading, `save_document` and `copy_document` under an import's name
meet it the same way, and A2 then trashes the import (not run).

The other way round is not closed either. `import_data` and the importer's naming treat a name as
taken by `name.sch`, not by a schematic's Data Set. With `p.sch`'s Data Set `run.dat`, `import_data`
with `name: "run"` was taken and wrote `run.dat` (`h_dsimport.py`), the file a Qucsator run of
`p.sch` writes over, and the name its ngspice run's dataset shares.

*Fix:* `keep_as` (and a schematic's new name) refuses a name an imported dataset has. The
importer's naming treats the Data Sets of the folder's schematics as taken, as it does their file
names.

**Fixed in `155f3d2`.** `keep_as`, `save_document` and `copy_document` refuse an imported dataset's name. `import_data` refuses a name that is a schematic's Data Set, and the importer's naming counts the folder's Data Sets as taken. Test: `theDataSetNamesTheResults`.

### A6. A net named GND: ground to the check, an ordinary node to SPICE OPUS

Without a ground symbol, a net labelled `GND` is netlisted as node `GND`, under ngspice and SPICE
OPUS alike (`h_gndcase.py`). ngspice reads any case of gnd as ground; SPICE OPUS reads only 0, and Xyce only
0 unless its ground synonyms are turned on. Check Schematic counts `gnd` in any case as ground
for every SPICE simulator, so with the ground not required it says nothing, and SPICE OPUS gets
a circuit with no ground. The fix of `d62605f` leaves `GND` among the voltages printed for SPICE
OPUS for the same reason, and its test says so. Not run: neither simulator is installed. (An
earlier probe, `h_gndsim.py`, seemed to show `GND` netlisted as 0. Its files `n_GND.sch` and
`n_gnd.sch` are one file on macOS's file system, so that row is void.)

*Fix:* the check's ground by name should follow the simulator: any case for ngspice, `0` (and
exactly `gnd`, which the netlist writes as 0) for the others.

**Fixed in `a1f731a`.** The check's ground by name follows the simulator: 0 and gnd for every one, gnd in any case for ngspice alone. Tests: `test_erc` `aNetWithTwoNamesIsOneNode` (a GND label under ngspice and SPICE OPUS), `aNetNamedAsGroundIsNotPrinted`.

### A7. A Data Set or Data Display out of the schematic's folder

A schematic's Data Set is taken as a path beside it, whatever it holds (`h_dsname.py`):
- `../up.dat`: the run wrote `../up.dat.ngspice`, out of the schematic's folder, and the trace read
  it.
- `sub/run.dat` wrote into the subfolder.
- `run` and `run.csv` gave `run.ngspice` and `run.csv.ngspice`. The diagram reads them, but the
  Data and Export tabs list `*.dat*` only, so neither shows them.

A schematic from elsewhere can so choose where a simulation writes. ngspice's dataset gets
`.ngspice` added. Qucsator's gets no suffix, so `../../notes.txt` would be written over (not run:
Qucsator is not installed here).

The Data Display is a path too (`h_dplpath.py`). `add_diagram` with `document: "data_display"`
on a schematic whose Data Display is `../out.dpl` answered "../out.dpl could not be opened", and
left an empty `out.dpl` out of the folder. With `amp.txt` it answered the same and left an empty
`amp.txt`.

*Fix:* a Data Set and a Data Display are names in the schematic's folder (no `/`, no `..`); a
Data Set ends in `.dat` (the dialog can add it). A data display that cannot be made writes
nothing.

**Fixed in `155f3d2`.** A Data Set or Data Display is a name beside the schematic: folders and `..` are left out as it is read or set (the settings dialog shows it so), and a Data Set ends in `.dat`. A Data Display that is no `.dpl` is refused by the tools, and no file is made or opened for it. Test: `theDataSetNamesTheResults`.

### A8. Two ports on one net: the `.SUBCKT` line names a node twice

A subcircuit `thru` passes a signal through: ports P1 and P2 wired together, and R1 from them to
P3. Placed with 1 V on its first pin and a load RL of 1 kOhm on its second, RL should carry 1 mA.
The netlist (`h_twoports.py`):

```
.SUBCKT thru P1 P1 P3
R1 P1 P3  1K
XSUB1 _net0 _net1 0 thru
```

ngspice takes the repeated node without a word and ties only one of `_net0`, `_net1` to it. The
run "succeeded" with RL's current 0 and R1's 1 mA. Check Schematic said nothing of the
subcircuit. The port names come from `nameUnlabelledPortNets` (ours, `9d583cf`). Before it the
line would have read `_net0 _net0`, so the cause is older: each port is a pin of the line, and
two on one net cannot both be.

Two ports on the subcircuit's own ground give `.SUBCKT thru P1 gnd gnd` (`h_portground.py`). Its
run could not tell right from wrong (the load there carries nothing either way), so it is only
said.

*Fix:* the netlister joins a second port on a net with a 0 V source (or `R 0`) from a node of its
own, and Check Schematic notes two ports on one net.

**Fixed in `a1f731a`.** Each port of a subcircuit is a node of its own on the `.SUBCKT` line: a port on ground or on another port's net gets `_port<n>_<k>`, joined to the net by a 1e-12 Ohm resistor (a 0 V source made a loop of sources when the instance grounds the pin too). Test: `test_symbol_pins` `eachPortIsANodeOfItsOwn`.

### A9. A port's net named after it: ground, or another net in another case

`nameUnlabelledPortNets` (`9d583cf`, ours) names a port's unlabelled net after the port, so the
`.SUBCKT` line and the symbol agree. It checks that the name is a plain word and not taken. It does
not check that the word is ground's (`h_portgnd.py`):
- **gnd**: a subcircuit of R1 between ports P1 and `gnd` netlists as `.SUBCKT sub P1 gnd`,
  `R1 P1 0`.
- **GND**: `.SUBCKT sub P1 GND`, `R1 P1 GND`, which ngspice reads as ground.

The parent puts 5 V on P1 and 2 V on the other pin, so R1 should carry 3 mA. Both ran with 5 mA:
inside, the pin is the global node 0, and the parent's 2 V never reaches R1. Before `9d583cf` the
net was `_net1` and the circuit right. A pin named GND is common on a chip's model, and nothing
stops renaming a port so. Renaming a port to `0` was refused. The last hunt's A1 kept ground's
names out of `create_subcircuit`'s port names, but not out of this one.

The check that the name is not taken is case-sensitive, and ngspice reads names without case
(`h_portcase.py`). A subcircuit of R1 from P1 to a net labelled `p1`, and R2 from there to P2,
netlists as `.SUBCKT sub P1 P2`, `R1 P1 p1`, `R2 p1 P2`. ngspice takes `P1` and `p1` as one node,
so R1 is shorted. With 2 V across the subcircuit it ran with R1 at 0 A and R2 at 2 mA, where each
should carry 1 mA. Check Schematic said nothing: its case rule compares labels with labels, not
with the names ports give.

*Fix:* `nameUnlabelledPortNets` leaves out the names `set_label` refuses (gnd in any case, 0,
`net<n>`, `_net<n>`), and compares names as the simulator does (without case for the SPICE ones).
Check Schematic should note a port named gnd: the pin is ground inside under ngspice.

**Fixed in `a1f731a`.** A port's net is named after it only when no other net has that name in any case, and never gnd in any case or a generated name; the check says why such a pin gets a generated name. Test: `test_symbol_pins` `aPortIsNotNamedAsGroundOrAsAnotherNet`.

## B. Data out and in: the Export tab, and the import

### B1. Memory and time

The exporter was run on a dataset of 500,000 points of time and 4 variables over it: 2.5 million
values, a 47 MB dataset (the harness in `h_big.patch`).

| format | output | time | peak memory |
|---|---|---|---|
| CSV | 17.6 MB | 0.56 s | 809 MB |
| text in columns | 21.5 MB | 0.44 s | 316 MB |
| NumPy | 3.0 MB | 0.27 s | 162 MB |
| Qucs-S dataset | 22.6 MB | 0.34 s | 164 MB |
| Excel workbook | 14.6 MB | 5.3 s | 1.6 GB |

Reading the dataset alone took 133 MB. CSV and TSV go through the spreadsheet module: a
`sheet::Cell` for each value (several strings each), then `csvText` copies the sheet to trim it.
That is about 270 bytes for each value, 38 times the file. A workbook takes about 600 bytes a
value. At Excel's own limit, a million rows of 20 columns, that is about 12 GB. The export runs on the GUI thread, with no progress and no way to cancel.

*Fix:* write CSV, TSV and text as a stream, a row at a time. Write a workbook's sheet XML
straight from the values, not through `Sheet`. Move a large export off the GUI thread, with a
progress dialog.

**Fixed in `003aac6`.** A table is written row by row: CSV, TSV and text straight to the file, a workbook's sheet XML from the values (`sheet::xlsxOf`). On 500,000 points of 4 variables, read into memory as the hunt measured: CSV 1025 MB to 198 MB, text 537 MB to 213 MB, a workbook 1826 MB to 477 MB. Written to a file, CSV and text take nothing over the dataset read. A large zip part is packed at zlib's own level: the workbook took 9.3 s, now 3.3 s, 1.4 % larger. An export that takes a while shows how far it is, with Stop; stopped or failed, the file there stays as it was. It still runs on the GUI thread. Tests: `test_data_export` `aLargeTableIsWrittenAsItGoes` (the files are the same as the spreadsheet's writers make), `aLongExportCanBeStopped`.

### B2. The file's name does not choose its format

`DataExportPanel::exportFile` takes the format from the combo box, and adds its suffix only to a
name without one. Typed as `out.xlsx` with CSV chosen, the file is CSV text that Excel refuses.
`dataexport::formatOfSuffix` was written for this and nothing calls it.

*Fix:* a suffix of a known format chooses it (said in the status); an unknown one gets the
chosen format's suffix added.

**Fixed in `003aac6`.** A name that ends in a format's suffix chooses that format, and the status says so; a name of no format's gets the chosen one's suffix, and the dialog asks before replacing that file. Test: `test_data_export` `theExportTab`.

### B3. A dataset exported beside the schematic is not in the Data tab

Exported as a Qucs-S dataset into the schematic's folder, the new `.dat` appears in the Export
tab's own list (it refreshes itself), but the Data tab's list is built when the dialog opens, and
the Export tab tells it nothing. (Reading; the Import tab emits `datasetsChanged` for this.)

**Fixed in `003aac6`.** The Export tab says when it wrote a dataset beside the schematic (`datasetsChanged`), and the dialog lists it in the Data tab. Test: `test_data_export` `inTheDiagramsDialog`.

### B4. Importing a large CSV goes through the cells too

`import_data` of a CSV of 500,000 rows and 5 columns (26 MB) took 2.3 s, and so did reading it
again. The server's resident memory was 1.15 GB afterwards (`h_bigimport.py`). The importer
reads a CSV or TSV with `sheet::readCsv` (`dataimport.cpp`): a `Cell` of several strings for each
value, as B1's export writes it. The Import tab does the same on the GUI thread.

*Fix:* parse a table's numbers as a stream into the columns, without the workbook model.

**Fixed in `003aac6`.** A CSV or TSV is read as rows, a row of numbers kept as numbers; text in columns too. 200,000 rows of 5 columns: 409 ms and 384 MB before, 159 ms and 107 MB now, read the same. Test: `test_data_import` `aCsvIsReadAsRows`.

### B5. For an imported dataset, the file proposed is its source

`DataExportPanel::exportFile` proposes `<dataset>.<suffix>` in the schematic's folder. The Import
tab names a dataset after its file, so for `bench`, imported from `bench.csv` beside the schematic,
Export with CSV proposes `bench.csv`: the measurement itself. The system's dialog asks before
replacing it, and that is all; `exportTo` refuses only the dataset being exported. Written over
with fewer columns chosen, or another x, the original is gone. (Reading.)

*Fix:* refuse an imported dataset's source as the target, as the dataset itself is refused, and
propose another name (`bench_export.csv`).

**Fixed in `003aac6`.** The file an imported dataset of the folder came from is refused as the target, and the file proposed for an imported dataset is `<name>_export`. Test: `test_data_export` `theExportTab`.

## C. The tools

### C1. A trace with no data reads every imported dataset in full

When a trace of a variable named alone shows nothing, the answer looks for an imported dataset
that has the variable (`importedWith`): it reads each imported dataset of the folder, up to 20,
in full, for every such trace. `whyNoData` does it too, and every listing of traces calls it. With
three imported datasets of 47 MB beside a schematic of no dataset yet (`h_slow.py`):

| call | time |
|---|---|
| `add_diagram` of 8 traces with no data | 9.8 s |
| `get_schematic` | 4.9 s |
| `reload_data` | 4.9 s |

Each was on the GUI thread.

*Fix:* read only the variables' names (the `<dep name` lines) for the hint, and once per call,
not per trace.

**Fixed in `5c89ca0`.** The hint reads the imported datasets' variable names from their `<dep` lines alone, kept while the file is as it was. 8 traces beside an 8 MB import: 526 ms before, 3 ms now. Test: `test_qucs_control` `theToolsTakeBackWhatTheyGive`.

### C2. `run_script`'s `qucs.call` refuses `max_chars`

The instructions say every tool takes `max_chars`; a direct call and a `batch`'s calls do. Inside
`run_script`, `qucs.call("get_schematic", {max_chars: 300})` fails: "get_schematic takes no
max_chars: nothing was done". The same happens for the text `"300"` (`h_maxscript.py`).

*Fix:* take it out of a script's calls as `batch` does (`readMaxChars`, `maxCharsRefusal`).

**Fixed in `5c89ca0`.** `qucs.call` takes `max_chars` as a batch's calls do, and refuses one that is not a number. Test: `theToolsTakeBackWhatTheyGive`.

### C3. `get_dataset` refuses the trace name it gives

For an imported dataset, `get_dataset` lists each variable with `"trace": "m:gain"`. Handed
back, `get_dataset` with `variables: ["m:gain"]` answers "m.dat has no variable m:gain. It has:
f, gain." A simulation's trace name, `ngspice/tran.v(out)`, is taken (the prefix is left out).
On the schematic, `variables: ["m:gain"]` says there is no dataset of `p.sch` yet
(`h_tracename.py`).

*Fix:* `resolve` takes `name:variable` when the dataset is `name`'s, and `get_dataset` of a
schematic follows `name:` to that dataset.

**Fixed in `5c89ca0`.** `Dataset::resolve` takes `name:variable` when name is the dataset's own. `get_dataset` of a schematic whose variables are all of one other dataset (`m:gain`, `ngspice/run1:v(out)`) reads that dataset, and says so when there is none. Test: `theToolsTakeBackWhatTheyGive`.

### C4. `import_data` reads a schematic as data

`import_data` with `file: "p.sch"` made `p_2.dat`, "text" with the columns B to F: the
schematic's coordinates. It noted "22 lines among the numbers that were not numbers were left
out" (`h_import.py`). The Import tab's file filter keeps a schematic out; the tool does not.

*Fix:* refuse Qucs-S's own documents (`.sch`, `.dpl`, `.sym`) as data.

**Fixed in `5c89ca0`.** The importer refuses `.sch`, `.dpl` and `.sym` ("p.sch is a schematic of Qucs-S, not data"), for the tool and the Import tab alike. Test: `theToolsTakeBackWhatTheyGive`.

### C5. `set_subcircuit_parameters` takes any default without a space

`Rs=-`, `Rs=1k;` and `Rs=1,5` were each taken and written to the `.SUBCKT` line (`h_subpar.py`).
With an instance that sets its own value, ngspice ran; one that takes the default would get `-`
(not run).
`temp=27` and `time=1` were taken, and ngspice took them too.

*Fix:* a default reads as a value (`units::read`), an expression in braces, or a parameter's
name.

**Fixed in `5c89ca0`.** A default is a number (a scale and unit letters after it), an expression in braces or quotes, or a name. Test: `theToolsTakeBackWhatTheyGive`.

### C6. `make_symbol` takes a prefix with `;`

`prefix: "X;Y"` was taken: the next instance is `X;Y1`, netlisted as `X;Y1 _net0 _net1 sub`.
ngspice reads from the `;` as a comment. `é` and `1X` were taken too (`h_prefix.py`); `A B` and
the empty prefix were refused.

*Fix:* a prefix is a SPICE word: letters, digits and `_`, beginning with a letter.

**Fixed in `5c89ca0`.** A prefix is a letter, then letters, digits and `_`. Test: `theToolsTakeBackWhatTheyGive`.

### C7. `set_dialog` on a tree: what is out of range is not said

On Find and Replace's result tree, a row out of range (`[99, 0, true]`), a column out of range,
or any row of a tree with no rows was answered "Not done: ... does not take [99,0,true]". The
answer gives the value back, and the reason is left out (`h_treefuzz.py`, 60 rounds, no crash).

*Fix:* "the tree has 1 row (0)", "a row is [row, column, checked or text]".

**Fixed in `5c89ca0`.** A row out of range says how many rows the tree or table has ("the tree has 2 rows (0 to 1)"), a column its columns, and a value of the wrong form what a row is. Test: `test_qucs_control` `aDialogsTreeIsReadAndChecked`.

### C8. `import_data` lists every column

A CSV of 5,000 columns and 3 rows imported fine, but the answer was 112 KB: `columns` held all
5,001 names, while `variables` and `traces` stop at 100 (`h_wide.py`). Nothing cut it. An agent's
context takes it whole, for a list it needs only to choose x from.

*Fix:* cut `columns` as the others are cut, saying how many were left out, or give it only when
x was not found.

**Fixed in `5c89ca0`.** `columns` is cut at 100, and `columns left out` says how many more there are. Test: `theToolsTakeBackWhatTheyGive`.

### C9. A kept run's trace names name the current run

After `simulate` with `keep_as: "run1"`, `get_dataset` of `run1.dat.ngspice` lists its variables
with the traces `ngspice/ac.v(in)`, `ngspice/ac.v(out)`, ... (`h_keptrace.py`). Handed to
`add_trace`, `ngspice/ac.v(in)` plots the schematic's own dataset, `AC_Passive_analysis.dat.ngspice`
here, and not the kept run. After a change and a new run, a "before and after" plotted from these
names shows the new run twice. `get_dataset` adds the simulator's prefix from the file's suffix,
but not the dataset's name when it is not the schematic's own (C3 is the same for an import).

*Fix:* the trace of a dataset other than the schematic's own is `<prefix>/<name>:<variable>`.

**Fixed in `5c89ca0`.** A dataset that is no schematic's Data Set (a kept run, an import) has its traces named `<prefix>/<name>:<variable>`; the schematic's own, open or not, keep `<prefix>/<variable>`. Test: `theToolsTakeBackWhatTheyGive`.

## D. Minor

### D1. Vac_SPICE counts as an AC source whatever its text

`acIssues` treats any Vac_SPICE as driving an AC analysis. It checks S4Q_V and S4Q_I for an `AC`
word but not Vac_SPICE, whose text is the user's too: `SIN(0 1 1k)` alone has AC magnitude 0, and
the warning is not given (`erc.cpp`, reading). The other sources (Vpulse, Vrect, Vexp, SFFM,
...) write `AC 0` and are rightly not counted; none of the 251 examples is warned by either new
rule.

**Fixed in `a1f731a`.** Vac_SPICE is counted by its text's AC word, as S4Q_V is. Not tested: the part is not built in this tree.

### D2. The kept netlists are never removed

Each ngspice run, from the window too, keeps its netlist in the cache (`netlists/<dataset>-
<key>.cir`), one for each dataset path ever simulated. `clean_scratch` does not remove them, nor
does anything when a schematic or dataset is deleted.

**Fixed in `155f3d2`.** A kept netlist records its dataset's path, and those of datasets that are gone (and of the stamp of before, with no path) are removed when another is kept. `clean_scratch` removes those of the datasets it trashes. Test: `theDataSetNamesTheResults`.

### D3. In the shared folder, `clean_scratch` trashes everything there

With no project open, schematics share one scratch folder. When the last run there was this
schematic's, `clean_scratch` trashes every file in the folder: what other schematics' runs left
under other names too, not only the last run's files.

**Fixed in `155f3d2`.** In the shared folder only the last run's own files go (`spice4qucs.*`, `log.txt`). Test: `test_qucs_control` `projectsAndCopiesAreTended`.

### D4. A test of `d62605f` grounds nothing

`test_erc`'s `aNetNamedAsGroundIsNotPrinted` places its ground symbol at 50, 90, the middle of the
wire from 0, 90 to 100, 90. A pin on a wire's middle is not connected ("the ground at 50, 90 is
connected to nothing"), so the cases "with a ground symbol" are those without one. The fix
holds (the probes ran with one connected), but the test does not show it. The same fixture is
in `aNamedGroundAndOneSwitchedOff`.

**Fixed in `a1f731a`.** The fixture's ground symbol is at the wire's end, connected.

### D5. The SPICE file part's Preprocessor starts the wrong program (upstream)

`SpiceFile::createSubNetlistPlain` builds the command as `script << interpreter; script << script;
script << file`, from `script = ["ps2sp"]` (or spicepp.pl, spiceprm). Appending the list to itself
gives `["ps2sp", "perl", "ps2sp", "perl", file]`. The program started is `ps2sp`, not perl, which
is not on `PATH`; were it there, its arguments would be wrong. So the option has never worked
(reading `components/spicefile.cpp`, unchanged since the source was vendored). The choices are
fixed, so no command of the file's own runs.

*Fix:* `script.prepend(interpreter)`, and `cmd` the interpreter.

**Fixed in `8f88220`.** `SpiceFile::preprocessorCommand` builds it: Perl, then the script (the one beside Qucs-S, else found on PATH with `perl -S`), then the file (and the output, for spiceprm). Test: `test_erc` `aSpiceFilesPreprocessorIsPerlRunningItsScript`, which builds the command and runs nothing.

## E. Security

### E1. `simulate` runs a schematic's commands, unannounced

A schematic can carry commands that run when it is simulated:
- **A System command part** (`CMD`, upstream's): Qucs-S runs its text in a shell after each run
  (`QucsApp::runPostSimCommands`). With `console` set to yes, it opens Terminal.
- **ngspice's `shell`** in a custom simulation (`.CUSTOMSIM`), a Nutmeg script, or any text that
  reaches the `.control` section.
- **An Octave script**: with the Document Settings' "run script after simulation"
  (`<RunScript=1>`), the script the schematic names (`<Script=...>`, any path) runs in Octave after
  each run (`slotAfterSpiceSimulation`). Not run here: Octave is not installed.

Through Claude's tools, `open_document` and `simulate` on a schematic with `touch <marker>` in a CMD
part made the marker (`h_cmd.py`). So did `shell touch <marker>` in a custom simulation
(`h_shell.py`). Neither answer mentioned a command. `check_schematic` said nothing, and
`get_netlist` showed the `shell` line among the others. The client asks before `simulate` unless the
user allowed it (it is not read-only), but what it shows is a simulation, not the commands it will
run. In the window, a person presses Simulate on a file they opened. Through the tools, an agent can
open and simulate a project's or a downloaded schematic as a matter of course, and whatever it
carries runs with the user's rights. The probes ran in a scratch folder, and the command only
touched a file there.

*Fix:* Check Schematic tells every command a run would execute: a CMD part, and a `shell` (or
`system`, `!`) line in ngspice text. `simulate` (and `tune`, which simulates again and again)
refuses a schematic that has one unless asked (an argument, which the client's permission prompt
then shows), and says what ran.

**Fixed in `8f88220`.** `erc::commandsRun` lists what a run executes besides the simulator: an active command part's lines but its comments, a line of ngspice text that starts with `shell`, `system` or `!` (a custom simulation's, Nutmeg's, `.spiceinit`'s), and the Octave script of "run script after simulation". Check Schematic warns of each at its part. `simulate`, `tune` and `tune`'s knobs refuse a schematic with one before anything else, naming them, unless `allow_commands` is given. The runs then carry it, and the check's warnings in the answer say what ran. Not covered: a `.control` block in an included file, and code loaded by ngspice (`pre_osdi`, `codemodel`). The window's own Simulate is as before. Tests: `test_erc` `commandsARunExecutesAreSaid` and `test_qucs_control` `commandsAreNotRunUnasked`. These only read and refuse: the schematic has no analysis, so even a run asked for stops before anything runs.

## What was found right

- `import_data`:
  - refuses names that are not words, a schematic's name, a simulation's dataset's, a missing
    file, a folder, an empty CSV, `reload` and `remove` together, and an untitled schematic;
  - nothing it was given reached outside the workspace (`../x.csv`, `/etc/passwd` as names);
  - quoted CSV headers with commas (`"S[1,1]"`) are read and made safe.
- The trace rules: an imported dataset, a plain `name.dat`, a kept run, and `qucsator/` each named
  as meant (probes and s10).
- The fix of `d62605f`: a net labelled 0 or gnd is netlisted as 0, and not printed, under ngspice
  (probes).
- The exporter: 25,542 exports of random datasets with no crash.
  - Every workbook read back with a sheet per table.
  - Every NumPy array's data matched its header's shape.
  - Every dataset read back with its values, except where the random dataset itself named two variables
    alike (the reader keeps the last).
  - 3,258 exports were refused, each as designed: variables of several sweeps for a format of one table,
    or nothing chosen that fills a table.
  - The CSVs read back, but for all-NaN columns, which the importer leaves out as it says.
  - NumPy and pandas read the samples (the feature's commit).
- The two new rules of the check: no false alarm on the 251 examples. `ACmag` is netlisted
  through `normalize_value`.
- Verilog-A outside a project: only the modules the netlist uses are built.
- Subcircuit parameters: instances follow by name; ngspice takes `temp` and `time` as
  parameter names.
- Previews: a diagram previewed onto an open data display with a diagram of its own leaves it as
  it was.
- The scenario script: s1 to s10, 77 checks, none failed.

## The scripts

Beside this report, in `2026-09-30-data-and-wishlist/`:
- `mcp.py`: the client; `QUCS_PROBE_ROOT` names the scratch folder.
- `h_*.py`: the probes named above. `h_argfuzz.py <app> <seed> <seconds>` is the argument
  fuzzer; `h_treefuzz.py` is the dialog tree's.
- `h_exgen.py`: the random datasets.
- `h_big.patch` and `h_exfuzz.patch`: test slots for `test_data_export` (memory and time;
  the export fuzz), applied for the hunt and taken out again.
- `notes.txt`: the notes taken during the hour.
