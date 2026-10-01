# Feature gaps: Qucs-S's MCP tools, the assessment after a day's use

*1 October 2026 — Qucs-S 26.1.4.*

Claude wrote an assessment of the tools after the day of 30 September
(`qucs-s-mcp-api-assessment-2026-10-01.md`). In that day a CSV was plotted, a
Touchstone target was fitted with ngspice's `optimize`, and the fit was
plotted and exported. It was written at `6511b15` (the Export tab), from
the session's files in `~/QucsWorkspace/opamp741_prj` (`pi_fit.sch`,
`pi_target.s2p`, `csv_plot.sch`, `dummy_data.csv`).

Each item was reproduced first, through `qucs-s --mcp-server` with its own
workspace, settings and HOME, on copies of those files. The first item was
done before the assessment arrived (`98db38c`). The others are in
`3bb5927`. Each change has a test, and each was broken on purpose to see
its test fail.

## Where it fell short

| Assessment | Now |
|---|---|
| **1. No `export_data`.** The Export tab cannot be reached from the tools: no menu action opens a diagram's dialog, and its Export button asks for a file in a dialog `set_dialog` cannot answer. | `export_data` (`98db38c`). The reviewer's arguments are all there: `path`, `dataset` (a file, or a name `keep_as` or `import_data` gave), `variables`, `diagram` (with `traces` to choose some), `format` (csv, tsv, text, xlsx, npz, dataset), `complex` (real_imaginary, magnitude_phase, db_phase) and `save_as`. A diagram's traces of a run and of a measurement are two datasets, so they are written in two calls, and the refusal says which traces belong to which dataset. Variables may be trace names (`ngspice/run1:v(out)`, `m:gain`) or expressions (`db(ac.v(out)/ac.v(in))`). The answer is what the reviewer asked for, from `Written`: each table's rows and columns, or the arrays, and the notes. CSV, TSV and text hold one table, as the tab enforces. As in the tab, a format's suffix in `save_as` gives the format. Neither the dataset nor a file a dataset was imported from is written over. A file written over is said, and `undo` with `files` puts it back. Tried on a simulated RC low-pass: its Bode plot exported as CSV in dB and phase puts −3 dB at 1587.8 Hz (1591.5 by the formula, on 10 points a decade). |
| **2. Complex traces plot magnitude only.** Phase needed a NutmegEq on the simulation side, and on the target side a second CSV of phases computed outside. `y_axis.units: "dB"` relabelled the axis and did not convert. | A trace's **`part`**: `db` (20 log10 of the magnitude), `phase` (degrees, −180 to 180), `magnitude`, `real`, `imaginary`, or `auto` (as before: a complex value's magnitude, a real one as it is). `add_diagram`'s trace objects, `add_trace` and `edit_trace` take it, and `get_schematic` gives it. It is applied as the data is read (`Graph::takeValuePart` in `Graph::loadDatFile`). So the curve, its markers ("phase(ac.v(out)): 90"), a table and the axis's range all show the part. The legend, an axis's automatic label, a table's column and the dialog's trace list name it `dB(...)`, `phase(...)`. The diagram dialog has a **Shows** box for it. It is kept as a tenth field of the trace's line, written only when it is not `auto`, so older versions read the file and plot the magnitude. A Smith chart, a polar or a locus diagram plots the complex value itself, and `part` is refused there. **`units`**: on a logarithmic axis, units label the numbers in dB of the values (20 log10). On a linear axis they do nothing, and the window greys the box out there. That is what the reviewer met. The schemas now say so. `add_diagram` and `edit_diagram` say it when `units` are on a linear axis ("'log': true reads them so, or a trace's 'part': 'db' plots them in dB"), or that the units can go when the traces are in dB already. The reviewer's target, `target:S[2,1]` with `part` `db` and `phase`, plots both on one diagram with no equation and no second import. |
| **3. Repeated properties have no JSON form.** `.NGOPT`'s `Knob=` and `Target=` had to be written as `.sch` text. | Reproduced as worse than written. The tools took a list for such a property all along (`setRepeated`), and `get_schematic` gives one. But the schemas of `add_component` and `edit_component` declared every value a text, and the server's own type check refused the list before the tool saw it. Now a value is a text or a list of texts. The schemas say which properties repeat (`.NGOPT`'s Knob and Target, NgSweep's Record and Vs, a Monte Carlo's Record and Spec, an optimization's Var and Goal) and give an example. A list replaces every property of that name. `describe_component_type` gives a type's **repeated properties**, each with its fields, an example and how to give it: "kind\|name\|initial\|low\|high - kind dparam (a .PARAM's or an equation's parameter), param (a device's: R1, @m1[w]) or mparam (a .model's: @dmod[is])". A list was chosen over typed `knobs` and `targets`: it covers all four blocks at once, and `get_schematic` already reads it back in that form. |
| **4. Optimizer results are not surfaced or applied.** The found values were only in the log. Qucs-S wrote them into the knobs' initial values (hence "changed while it ran") but not into the `.PARAM` the parts read. | `simulate` gives **`optimum`**, one entry for each active `.NGOPT`: its summary ("converged, sum-sq residual = 5.23e-09 ... after 20 evaluations"), each knob's value found (`{"Cp": "31.831p", "Lp": "159.155n"}`) and any notes ngspice gave (a bound reached, interrupted). With **`apply_optimum`** the values are written where the knobs name them, one undo step: a dparam into the `.PARAM`, `.GLOBAL_PARAM` or equation block that defines it; a param into the part's value (`R1`) or property (`@m1[w]`). `applied to` says where (`"Cp": "SpicePar1.Cp"`). What cannot be written is said, for example a `.model` parameter, which its model card holds. Without it the answer says the parameters were not changed, and how to change them. The dataset is the optimum's, so after applying, the netlist kept for it is the one a run would be given now, and `get_dataset` does not call it stale. **"Changed while it ran"** came from Qucs-S writing the knobs' initial values, an edit counted as the user's ("1 by you"). That edit is now the simulation's own (`QucsDoc::kSimulation`): no note while the run is going, and "the simulation (an optimizer's result written into its knobs)" in `since_last_call`. The same goes for the Optimization component's variables. On the session's `pi_fit.sch` from 20 pF and 100 nH: 31.831 pF and 159.155 nH, written into `SpicePar1`, and undone in one step. |
| **5. Pin orientation after rotation is undocumented**; `connect` drew round the capacitor and said only "going over no other pin or wire". | `describe_component_type` takes **`rotation`** and **`mirror`** and gives the pins as a part placed so has them: the part is turned as a file has it, mirrored first. Without them it adds the rule: each quarter turn moves a pin at (x, y) to (y, −x), so rotation 1 puts it at (y, −x), 2 at (−x, −y), 3 at (−y, x), and mirror puts it at (x, −y) first. The test checks the rule and the argument against a placed part for all eight orientations. **`connect`** now says when a pin faces away from the other end, with the rotation that turns it: "C1's pin 1 faces down, away from L1.1: the wire goes round C1. At rotation 3 (now 1) it faces L1.1". It also says when the wire crosses a part's body without a pin of it ("It crosses the body of R9 (no pin of it): move it, or give 'side' or 'via'"). |

## Smaller

| Assessment | Now |
|---|---|
| `get_dataset` refuses `db(S[2,1])`: "a ) is missing after db(". | What is in brackets is the name's, its comma too. `db(S[2,1])` and `phase(S[2,1])+1` evaluate, and so does an expression of `add_diagram` that names one. |
| `import_data` named a second import `dummy_data_2` without saying why. The first had been imported from another copy of the file. | When a file's name is taken, the answer's `named` says what by. Here it is "dummy_data is the dataset imported from dummy_data.csv: this one is dummy_data_2 ('name': "dummy_data" puts it in that one's place)". It may also be a simulation's dataset, a schematic, or a schematic's Data Set. |
| `edit_trace` answered `changed: false` without saying why. | That was a `preview`'s answer. `edit_trace` itself now says when nothing changes, with `changed: false` and "Nothing changed: trace 1 is so already". When the variable asked for is written another way there, it says how ("'variable' X is written Y here, which it is"). No undo step is made. |
| `edit_component` with `rotation` redraws wires ("6 wires drawn, 5 taken away"); a preview showing the new wires would help. | The change summary of a preview, of `since_last_call` and of a batch's answer lists the wires by their ends, when there are six or fewer: "2 wires drawn (220,160-220,200; 220,160-310,160), 2 wires taken away (220,260-310,260; 310,160-310,260)". |
| Diagrams default to `legend: off` in `add_diagram`, while the text-form diagram had a legend. | Not a difference between the routes. A diagram is made with no legend in the window, by `add_diagram`, and as a `.sch` line without the field. The session's text-form diagrams had the legend written into them (fields 3, 4 and 2). The schema says it is the default. `add_diagram` with more than one trace and no `legend` says so: "No legend (a new diagram's is off, in the window too): 'legend': "top_right" says which curve is which". |

## Tests

- **`test_qucs_control`:**
  - `aTraceShowsAPartOfItsValues`: each part's values as read, the marker, the file's field read back, Smith refused, the units and legend notes.
  - `aRepeatedPropertyTakesAList`
  - `anOptimumIsReportedAndApplied`: an RC fit through ngspice's optimize, skipped without it; not applied, applied, not stale, undone.
  - `pinsAreGivenTurnedAndConnectSaysWhy`: every rotation and mirror against a placed part, the reviewer's capacitor, a body crossed.
  - `theSmallerNotesOfTheAssessment`
  - `curvesAreExported`: item 1, `98db38c`.
- **`test_data_export`:** `aTracesPartInTheDialog`: the Shows box, the row's name, through OK, read as the phase.
- **`scripts/ci/check-tool-arguments.py`:** 71 tools, 0 disagreements.

Each fix was broken on purpose: 33 breaks in this round, and 17 for
`export_data`, and each was caught. Two were caught only after the test
was made stronger:
- A mirror left out moved no pin of a capacitor, whose pins are on its
  centre line. The test now turns an op-amp.
- The Optimization component's write into its variables was not covered
  by the tools' tests. `test_optimization`'s `aDividerIsTuned` now checks
  that the edit is the simulation's.

The full suite passes (80), and so do the scenarios s1 to s10 (77 checks). `~/QucsWorkspace`
and `~/Library/Caches/qucs-s` are as they were before the runs; the session's files
were read, never written.

The tool list grew by 2,945 bytes, from 133,194 to 136,139. The 22 tools
sent every turn grew by 1,108 bytes, from 33,961 to 35,069. That is
`simulate`'s `apply_optimum` and its `optimum`, the repeated properties
in `add_component`'s `properties`, and the rest in tools found by search
(`describe_component_type`'s `rotation` and `mirror`, the trace tools'
`part`).
