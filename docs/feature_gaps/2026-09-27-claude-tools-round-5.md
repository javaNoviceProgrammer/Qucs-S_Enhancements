# Feature gaps: Claude's Qucs-S tools, round 5

*27 September 2026 — Qucs-S 26.1.3.*

Claude went back to `project1_prj/lc_lowpass_sp.sch` and the session's
earlier work (the emitter follower, a Monte Carlo, the Verilog-A driver)
with the tools of rounds 3 and 4. It wrote down:

- what those rounds fixed;
- comments on the new tools;
- what still needed `set_schematic`;
- a second wishlist and a top three:
  1. equation fields on `add_component` and `edit_component`;
  2. the field-count check in `set_schematic`;
  3. a diagram title that moves with the diagram;
- a third wishlist, "beyond editing".

Each wish was checked against the source. Some were already there, since
round 4 ([2026-09-27](2026-09-27-claude-tools-round-4.md)): the session had
run a build from between the rounds. The tables say what was done.

## Comments on the new tools

| Comment | Now |
|---|---|
| Run `check_schematic` inside `simulate`, its warnings first | `simulate` runs Check Schematic before the run. Its errors and warnings are in the answer (`schematic check`) and head the run's log (`last lines` begins with them). When the run cannot start, they follow the error. |
| `export_image` of a diagram says `pixels: [-1, -1]` | The size was measured after the export's own selection was undone. It is now read from the file written, in every mode. |
| A diagram's title should travel with it | Diagrams have a `title`: drawn centred above the frame and counted in the diagram's bounds, so it is selected, moved (`move` too) and exported with it. It is saved after the axis labels, only when there is one, so older versions read the line as before. `add_diagram` and `edit_diagram` take it, `get_schematic` lists it, and the diagram dialog has a Title field. |
| `list_documents`: the traces a dataset does not satisfy | A dataset lists the open diagrams' traces that read it but that it lacks (`traces it does not have`). An open schematic lists the traces whose dataset is not there at all (`traces without their dataset`, with the file each needs), which is the Qucsator-versus-ngspice mix-up. |
| `simulate` with a simulator for one run | `simulate` takes `simulator`: that one for this run alone, the setting left as it was (put back when the run ends, even after a timeout). A Qucsator run is waited for now, as a SPICE run is: its output, errors, dataset and traces without data. |

## What still needed set_schematic

| Wish | Now |
|---|---|
| Equation-kind components: NutmegEq, Eqn, a Monte Carlo's `Record=` and `Spec=`, extra `.OPTIONS` | **Top 1.** `add_component` and `edit_component` take `equations`: `{"gain_db": "db(v(out))"}`, or a list of `"name=value"` to keep their order. A value `""` takes one away; `replace_equations` makes them exactly those given. They apply to every equation block the component dialog knows: Eqn, NutmegEq, `.PARAM`, `.OPTIONS`, `.FUNC`, `.IC`, `.NODESET`, `.CSPARAM`, `.GLOBAL_PARAM`. The fixed fields stay where they are (NutmegEq's Simulation first, `.OPTIONS`' Xyce package first, Eqn's Export last). A new block's placeholder `y=1` is replaced. An ngspice Monte Carlo or corners block takes `records` and `specs` (`{"name": "gain", "expression": "db(v(out))"}`, `{"expression": "gain", "min": "19", "max": "21"}`). A property name an equation block does not have is refused with where equations go. |
| Hiding property text (`shown`) | Round 4: `edit_component` `shown`. |
| The text offset (tx, ty) | Round 4: `text_at`. |

## Wishlist, round two

| Wish | Now |
|---|---|
| Field-count check in `set_schematic` (top 2) | Round 4: a line with more values than its type has properties is refused, naming both counts. One with fewer is noted. |
| A marker at -3 dB of a reference | `add_marker` and `edit_marker` take `reference`: `peak` (the default), `dc` (the value at the curve's start), or a level such as 0 for a filter's spec in dB. |
| Diagram title field (top 3) | See the comments above. |
| NutmegEq Simulation: "ac" matched, "tran" did not | The value was matched as a prefix of the analysis block's name, so `ac` matched `ac1` but `tran` never matched `tr1`. It now also matches the analysis' kind by ngspice's name: tran, ac, dc or op, noise, sp, disto, pz, fft. `describe_component_type` says so. |
| Text tabs do not reload after a change on disk | The open documents' files are watched. A text document or schematic changed by another program (a script, a shell command, OpenVAF) is loaded again when it has no unsaved changes; a text keeps its cursor and scroll. One with unsaved changes is left, and the status bar says so. Claude's own edits were reloaded before; now any program's are. |
| Structured operating point, expressions, comparing runs | Round 4. |
| Monte Carlo helpers | `records` and `specs` by name (above). `get_dataset` reads a table (a `.csv`, `.tsv` or `.xlsx` file, a script's results or a Monte Carlo's workbook) as a dataset, each column of numbers a variable. The `distribution` measurement gives a family's summary: mean, standard deviation, median, 5th and 95th percentiles, a histogram, and with `level` the share at or above it. |
| Compile Verilog-A on demand | `build_verilog_a` runs OpenVAF on a `.va` and waits. It returns whether the file compiled and each error and warning with its line, column, source line and OpenVAF's marks. It also returns the `.osdi` and its modules. An open file with unsaved changes needs `unsaved: save` or `as_saved`. |
| Model parameters from OSDI | `describe_component_type` takes a Verilog-A module by name (looked for in the open documents, the project and the workspace) or by its `.va` or `.osdi` file. It gives the module's parameters (default, units, description, instance or model), from the compiled library, with defaults and descriptions from the source where the library has none. It also gives a `.model` card with the defaults. |
| `GND.1`, a warning when a part lands on a wire | Round 4. |
| `export_image` pixel size in every mode | See the comments above. |

## Wishlist, round three: beyond editing

| Wish | Now |
|---|---|
| **Watch mode**: the user's changes as a diff | Each conversation keeps each open schematic's state as of its last call (not for one of more than 5000 parts). What another hand changed since is told part by part: "R2: R 47k → 67k; C1 added (C, C=1 pF) at (400, 100); C1: moved from (400, 100) to (400, 300); a wire drawn; diagram 2: trace 2's look changed". It used to be told as "5 edits, read it again". |
| **Selection as input** | `move`, `delete`, `create_subcircuit` and `get_schematic` take `"selection": true`: the parts (and diagrams, paintings, wires) the user selected. `add_painting` takes `"around": "selection"`: a box about the selection, a text above it, an arrow to it, a brace beside it, or a dimension under it (the fields given stay). |
| **Pin to a folder or project** | A relative path is taken from the open project's folder first, then the workspace. A file's name alone (`amp.sch`) finds the open document of that name, and two of one name are told apart by path. `open_project` opens a project by name. |
| **Tune loop** (*the most useful*) | `tune` sets a component's property, simulates, measures, and repeats until the measurement comes to `target`. It searches `range` by false position with the Illinois rule, on a log scale over decades, in a few runs. It works within `tolerance` and at most `max_runs`. The measurement is any statistic or `get_dataset` measurement of a variable, or a node's DC voltage or a device's quantity from the operating point alone. It can also take `values`: each simulated and measured, as a table. The value found is set as one step to undo and simulated again, so the diagrams show it. |
| **Component search by value** | `find_library_component` searches Qucs-S's libraries, the user's `user_lib`, and the `.model` cards of the project's and workspace's SPICE files. It takes words, a kind (npn, pnp, nmos, pmos, njf, pjf, diode, or a Qucs model), and `near` values (`{"Bf": 200}`), nearest first on a log scale. Each find says how to place it: a library part with `add_component` type `Lib`, a SPICE model with its card. |
| **Datasheet reading** | `read_pdf`: a PDF's text page by page (`pages`: 7, `"2-5"`, [3, 4]), or `search` for a word or value, with the lines around each find. |
| **Measurements across a sweep or Monte Carlo** | A family measured (or read at `at`) comes with a `table`: a row for each curve (up to 1000), its parameters then each value, such as bandwidth against R or the overshoot of each sample. |
| **Eye diagram and FFT** | `get_dataset` measures `fft` (the spectrum of a transient resampled evenly with a Hann window: its strongest lines in amplitude and dBc, dc, the noise floor, the resolution). It also measures `eye` (a transient folded at `bit_period` from `offset`: the eye's height at the bits' centres, its width, the crossings' jitter peak to peak and rms, and the levels). |
| **Data display documents** | `new_document` with kind `data_display` opens a schematic's `.dpl`, or makes one when it has none. `add_diagram`, `add_painting` and `export_image` work on it by its name. |
| **Netlist import** | `import_netlist` makes a schematic of a SPICE netlist (text or a file). Each element becomes a SPICE part of its kind carrying its netlist text as written: `R_SPICE`, `S4Q_V` (SIN, PULSE and all), `NPN_SPICE` with its model, `NMOS_SPICE`, `DIODE_SPICE`, `VCVS` for a linear E, `SPICE_dev` for an X instance, `K_SPICE`, and so on. They are placed in rows, each pin's net a label on it and node 0 a ground. `.model` cards go into SpiceModel blocks. `.param`, `.options`, `.include` and `.lib` become blocks, and `.tran`, `.ac` and `.op` become analyses. `.subckt` definitions go into a library file beside the schematic, included. It netlists back as it came, and it simulates. |
| **Symbol drawing helper** | `make_symbol` draws a subcircuit's symbol: a box with each port on a side, from `sides` by name or number. Otherwise a supply goes on top, a ground or negative supply on the bottom, an input left and an output right, the rest left and right in turn. |
| **Rename a component with its references** | `edit_component` `rename` renames what names the part: traces, equations and a closed data display's file. That covers `i(v1)`, `@q1[ic]`, `v1#branch`, `V1.It` and `D1.Id`, keeping ngspice's lower case, as `rename_net` does for a net. |
| **Project scaffolding** | `new_project` makes a project and opens it (not while a document has unsaved changes). `copy_document` copies a schematic, an open one as it is, to a name, a path, a folder or a project, with its datasets and data display renamed with it and pointing at each other. `clean_scratch` clears a schematic's scratch files, and with `datasets` its datasets, to the system's trash. |
| **Undo history as text** | `undo_history` gives a schematic's steps in words, from the same diff as watch mode ("step 7: R2: R 47k → 67k"), up to where it is and those that can be redone. `undo` takes `to`, a step, to go back or forward to it, and every undo now says what it changed back. |

## Found on the way

- **`move`'s description** said dx and dy were grid steps. They are the schematic's units, as its own error message said.
- **An axis label with a double quote** broke the diagram's line in the file. It is refused, as a title's is.
- **A pinned conversation** gave its schematic's path to `new_document`. That now happens only for a data display, which is a schematic's.
- **A simulation that did not start** was reported with the status lines of the runs before it. It now reports its own, followed by the check's errors, which are why the GUI refuses to run.

## Tests

- **`test_qucs_control`** (60), new in this round:
  - equations, records and specs set by name, with a NutmegEq's `tran` in the netlist;
  - a rename taking its traces and equations along;
  - a diagram's title saved, bounded and read back;
  - `-3dB` below the peak, 0 dB and dc;
  - a diagram's picture with its size;
  - the check before a run, and a one-off simulator (a real Qucsator run where it is built);
  - traces a dataset lacks;
  - a text reloaded from disk, and not when it has changes of its own;
  - `build_verilog_a` with a stand-in OpenVAF, and a module described from its source;
  - `tune` with real ngspice: a divider to 2.5 V, a table of values, a target out of range;
  - a document by its name alone;
  - watch mode and the undo history in words, undo to a step and back;
  - the selection taken;
  - an FFT, an eye, a Monte Carlo workbook's distribution, a sweep's table;
  - a data display plotted on and exported;
  - a PDF read and searched;
  - parts found by their values;
  - a netlist imported, netlisted back and simulated with real ngspice;
  - a symbol on four sides;
  - projects made, a schematic copied with its results, scratch cleared.

  The new tools are also among the 600 calls of odd arguments.
- **`test_dataset`**: tables read as datasets (CSV with and without a header, TSV, a workbook), and a distribution.
- **GUI monkey**: its fake Claude calls equations, titles, reference markers, the undo history, the selection, the library search, renames, the FFT and eye, and a Verilog-A lookup. It was run under ASan/UBSan.

Ten of the fixes were broken on purpose to check that their tests fail:

- NutmegEq's match by kind;
- the picture's size;
- a rename's references;
- the title saved;
- watch mode's diff;
- the marker's reference;
- the check at the head of the log (caught once the test ran a real ngspice);
- a new block's placeholder equation;
- the reload from disk;
- the netlist's X letter.

All of them failed.
