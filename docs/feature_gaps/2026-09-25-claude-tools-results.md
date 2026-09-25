# Feature gaps: Claude's Qucs-S tools, reading results

*25 September 2026 — Qucs-S 26.1.3 (`45d0141`).*

Claude, working in the Qucs-S window on a photodiode testbench (ngspice,
a 20 ns transient), wrote down where its tools got in the way. The gist:
the tools were good at *editing* a schematic and poor at *reading
results*. Its seven suggestions, each checked against the source, and
what was done about them.

| # | Suggestion | Was | Now |
|---|---|---|---|
| 1 | `simulate` reports false failures | It looked for `name.dat` — Qucsator's dataset — whatever the simulator, and ngspice writes `name.dat.ngspice`: every ngspice run said `"succeeded": false`. | The dataset is the simulator's (`.dat.ngspice`, `.dat.xyce`, `.dat.spopus`, `.dat`). Success is the run's own account: it ran to its end, reported no error and the simulator exited with 0 (a new `SimulationRun::exitCode()`); a dataset not written is a note, not a failure. The variables it wrote are listed. |
| 2 | A way to read the dataset | Only screenshots; the numbers took `grep` on the file. | `get_dataset`: the variables; for those asked, statistics, samples over a range, values at given x (interpolated), and measurements on the full data — rise/fall time, overshoot, settling time, period, frequency, duty cycle, crossings, −3 dB bandwidth. Swept variables give a curve per value; complex ones magnitude/phase, dB/phase or real/imaginary. Names are found as people give them (`v(out)`, `out`, `ngspice/tran.v(out)`). To compare runs, `simulate`'s `keep_as` keeps a copy of a run's dataset (`run1.dat.ngspice`), which `get_dataset` reads by its file and a trace shows beside the current run (`ngspice/run1:tran.v(out)`). (`qucs/dataset.h`) |
| 3 | Diagram and trace tools | Only `set_schematic` with a hand-written `<Rect …>` line of ~30 positional fields — and the replaced diagrams lost their data until the document was reopened (`replaceContent()` never called `reloadGraphs()`). | `add_diagram`, `edit_diagram`, `add_trace`, `edit_trace`, and `delete` for diagrams and traces, by named fields (type, place, size, axes with label, log, limits, step, units; grid, legend; each trace's variable, color, thickness, style, axis, markers, auto colors; a table's precision and number form). Each trace reads its data at once and says its points or why it has none. `set_schematic` reloads the data too. `get_schematic` lists the diagrams numbered, with all of this. |
| 4 | `reload_data` | No action re-read a dataset. | `reload_data`, and *Simulation > Reload Simulation Data* for the user (`Schematic::reloadGraphs(bool force)`). |
| 5 | Net renames reach the traces | Renaming `out` to `out1` left the trace on `tran.v(out)`, blank. | `rename_net` renames the labels (or labels a `net3` of `get_schematic`), the traces of the schematic and its open data displays (a step to undo), a closed `.dpl` file, and the equations — `v(out)`, `vdb(out)`, `v(out,in)`, `out.v`, `out.Vt`, in the case the simulator writes. It refuses a name another net has. `set_label` warns when it leaves traces without their net; `simulate` lists the traces without data and why. |
| 6 | `describe_component_type` | Types only by name. | Properties in their `.sch` order with default, unit, meaning, shown or not, simulators; pins; naming; the netlist line the defaults make under the simulator in use; notes on traps (`Vpulse` is one pulse under SPICE, `Vrect` repeats; `Vdc` is nothing in AC; `.TR`'s Points is the print step). Equation blocks (`Eqn`, `NutmegEq`, …), which the library keeps outside its type hash, are found too — by `add_component` and `list_component_types` as well. |
| 7 | Netlist and console | *Show Last Netlist* opened a window the tools could not read; errors were the tail of stdout. | `get_netlist`: the netlist as a simulation would write it now, or the last one run (refused when the shared Scratch folder's last netlist is another schematic's), numbered if asked. `simulate`'s errors and warnings are structured: message, netlist line number and text, the part (a device inside a subcircuit is its subcircuit's part), the node. (`qucs/simulatorlog.h`) |

Found on the way: `delete` recorded two steps to undo (the schematic's
`deleteElements()` records its own), so one *Undo* after it did nothing.

Tests: `test_dataset` (the reader, name finding, every measurement on
curves with known answers, ngspice's error formats, net renaming in
variables); `test_qucs_control` drives every tool on a schematic with a
dataset, describes every type of the library (the netlist line of a part
on no schematic), gives the new tools 600 calls of random odd arguments,
and with ngspice installed runs a real simulation (kept, and plotted
beside the next) and one with an error. The GUI monkey's fake Claude
calls the new tools with hostile arguments too — and its round of tool
calls now starts at a different place for each seed: a walk sends only a
few prompts, so before, every seed repeated the first cases. A real
Claude (Haiku) given the task in five steps — simulate, measure, restyle
a trace, add a diagram, rename a net and simulate again — did it with
these tools alone.
