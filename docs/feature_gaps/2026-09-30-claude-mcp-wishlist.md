# Feature gaps: Qucs-S's MCP tools, the wishlist of 30 September

*30 September 2026 — Qucs-S 26.1.4.*

Claude wrote a state-and-wishlist file (`qucs-s-mcp-wishlist-2026-09-30.md`)
from the round-7 and round-8 verifications of `4c0e137` and `994c4ec`. It
has seven items and three smaller notes. Each was reproduced on `ebeb67f`
through `qucs-s --mcp-server` (isolated HOME and settings, the source's
library) before anything was changed. Three came out worse than the file
says, and the tables say how. Each change has a test and was broken on
purpose to see the test fail.

## The wishlist

| Wishlist | Now |
|---|---|
| **1. Subcircuit parameters as a first-class thing.** No tool adds or edits them; the only route is a raw file edit. | Half there: `edit_painting` on the symbol's name text took `parameters`, but the painting list called that text "only moved". Reproducing found two faults behind it. **`make_symbol` dropped them**: drawing the symbol again made a new name text, without the parameters or the prefix. **Instances took them by place.** A placed subcircuit keeps its values in order, and when its symbol changed, each value stayed in its place under the new name there. With [Rs, k] and an instance at Rs = 5k, k = 3, taking Rs away left the instance with neither: `Component::analyseLine` erased the last parameter it kept along with the rest. Taking all of them away took the instance's File property too. **Now**: a tool, `set_subcircuit_parameters`. `parameters` is `[{name, default, description, type, shown}]` or `"Rs=1k"`, each set or added by name; `remove` takes some away and `replace` makes the list exactly those given. A new one needs a default with no space (the `.SUBCKT` line takes `Rs=1k`). Names are SPICE words, not `File`, and not given twice in any case. It works on the symbol's name text, one step to undo, and says the `.SUBCKT` line's pairs. `make_symbol` keeps the parameters and prefix it had, or takes `parameters` and `prefix`. `get_schematic` lists `subcircuit parameters` in the same form. When the subcircuit is saved, instances in open schematics take its parameters **by name** (`Schematic::recreateComponent`): each keeps the value set on it, and a new parameter starts at its default. An instance in a schematic that is not open still reads its values by place when it opens. When a parameter is taken away or moved, the answer says so under `instances not open`. A divider `{Rs*k}` placed as X1 with Rs = 5k and k = 3 into 15k simulates 33.33 µA, 1/30k. |
| **2. Source netlisting traps.** Vpulse's `PULSE` has no period, so ngspice makes a step. Vac's AC magnitude is its transient amplitude. | **Vpulse**: under ngspice 46, a pulse from 1 to 5 µs over a 20 µs run was still 1 at 8 and at 15 µs. It is not quite a step: it repeats and is high most of the time. `describe_component_type` had promised "one pulse … stays at the first value after". Vpulse and Ipulse now write a period of 1e9 s: the same pulse is 1 at 3 µs and 0 at 8 and 15 µs. **Vac and Iac** gain `ACmag`, the AC magnitude (SPICE simulators; empty, the amplitude as before). It goes last, so a file of an older Qucs-S reads as it did. `U = 0, ACmag = 1` sweeps at 1 V with no transient. `check_schematic` warns when an AC analysis's only AC sources have magnitude 0: "AC1's only AC source is V2 (its U is 0, which with no ACmag is its AC magnitude too) of AC magnitude 0, so every voltage and current of it is 0". `describe_component_type` and `add_component` name the trap. Under Qucsator, Iac's netlist no longer carries IO and TD, which are SPICE-only, as Vac's never did. |
| **3. Nutmeg equation names that shadow a node.** | Worse than said. With `Tj1 = v(tj1) + 27` beside a node tj1 at 2 V, the dataset's **v(tj1) was 29 V**, not only missing Tj1. A later `ok1 = v(tj1) * 2` came out 58, not 4: the `let` wrote over the node's voltage. **Now a warning** (SPICE simulators): "NutmegEq1: its variable Tj1 is named as the net tj1: under ngspice the equation writes over the node's voltage (v(tj1) reads its result instead), and Tj1 is not in the dataset - name it otherwise (Tj1_eq)". It covers NutmegEq, and Eqn equations that read a voltage or current (one that does not is a `.param`, apart from the nodes). The names looked at are net labels, probes, and the axes `time` and `frequency`. |
| **4. Verilog-A outside a project.** | Reproduced: the template module built beside a schematic in the workspace, with no project open. The netlist had no `pre_osdi`, and the run failed on "Unable to find definition of model amp_model". That hint about `pre_osdi` is the ngspice build's own; Qucs-S said nothing. **The gate is gone**: the `.va` and `.osdi` files beside the schematic are built and loaded with or without a project. A project's subfolders are still walked, for a project only. `check_schematic` looks beside the schematic too. When a module is still not found, simulate's error gets a `hint`: where Qucs-S looked ("no project is open, so only the .va and .osdi files beside the schematic are (…)"), and, if the workspace has it elsewhere, which file ("defined in models/zzamp.va - not loaded … Put it there, or open its project"). The inverter of the template gives −1.00011 V. |
| **5. One staleness verdict.** Copies and `tune` with `apply: false` fall to the weak "edited after" wording. | A copy with a text added was told "edited … only the time tells". **`tune` with `apply: false` was worse**: nothing was said, though the dataset was of the last trial. A 1 kHz target left a 997.6 Hz bandwidth in the dataset with C1 back at 100 n. `tune` puts a value back without an edit, and the check compared netlists only after an edit. Outside a project there was a third gap. The netlist compared with was the scratch folder's last one, and every schematic outside a project shares that folder, so simulating a second schematic put the first on the weak wording. **Now** each ngspice run keeps the netlist it was given for its dataset, in the cache (`netlists/`, one for each dataset). It is stamped with the dataset's time and size, so a dataset written another way is not taken for it. `copy_document` carries it to the copy. The check compares whenever any part was made again since the run, a value put back included (`QucsDoc::touched()`). **`stale certain`** says which branch spoke: true when the netlists differ or a later run failed, false when only the time of an edit tells. The copy with a text added is not stale; with a value changed it is, certainly; after `tune` without `apply` it is, certainly. |
| **6. Coverage of the window side.** | Scenario **s9** in `scripts/mcp-e2e-scenarios.py`, 11 checks. Document Settings is opened by its menu action and read. Another tool is refused while it waits, and the schematic is unchanged. Cancel changes nothing and OK sets the dataset, which the next run then writes to. Find and Replace finds R1, and replaces nothing with the row unchecked, then 2k with it checked. The netlist says 2K, the dataset is certainly stale, the new run has half the bandwidth, and undo takes the replace back. **Found on the way**: `get_dialog` could not read a tree, which is where Find and Replace lists what it found. It now gives a tree's columns, rows and whether each is checked. `set_dialog` checks or unchecks a row (`[row, column, true]`) or sets a cell's text. |
| **7. Data displays.** A `.dpl` is write-by-hand. | `add_diagram` already worked on an open data display (`new_document` with `kind: data_display`), but nothing said so, and a `.dpl` not open was refused. Now `add_diagram` takes **`document: "data_display"`**: the schematic's `.dpl`, made when it has none. A `.dpl` named in `path` is opened (or made for its open schematic) for every diagram and painting tool, and the answer says so. One with no open schematic of its name is refused with why. |

## The smaller notes

| Note | Now |
|---|---|
| Keep `arrange feedback=below` on the 741 bench in the scenario script. | s8 checks it (Rg in U1's column with Rf, not the load's), with and without `straighten`. s8 passes. |
| `replace_component` could offer a best-guess pin map in its refusal. | Worse than said: from the built-in OpAmp, whose pins have no names, onto a library 741 whose pins do, it was **not refused**. It went by number: the OpAmp's + input (pin 2) to the uA741's pin 2, its output, and the output to INP, with nothing said. It is refused now when the numbers do not match the pins' roles and places. It says why ("2 (the + input) would go to 2 (OUT), not 3 (INP); 3 (the output) would go to 3 (INP), not 2 (OUT)") and gives the map to pass back: `'pins': {"1": "INN", "2": "INP", "3": "OUT"}`. The existing refusal, named pins onto unnamed ones, gives it too. The guess matches roles first: the OpAmp's by number, a library part's by its pin names (INN, IN_N1, VCC2, …). Then the same place on the symbol, read from the part as its type draws it, not as it was turned. Then the same side in order along it. `ua741(TI)`, whose numbers are its roles, still goes through by number. A turned OpAmp gives the same map. |
| Re-check "Rg left in the load column" on a bench that is not the 741. | The s8 bench was built with tl081(TI), OP07(TI), op27(mod), AD825 and the built-in OpAmp, and arranged with `feedback: below`, with and without `straighten`. In all ten layouts the columns were Vin \| U1, Rf, Rg \| RL, every net was kept, and the check was clean. Each simulates to 1.100 V (AD825 1.087 V). Nothing to change. |

## Tests

- **`test_qucs_control`**, new: `subcircuitParametersAreSetAndFollowByName`, `sourcesNetlistAsMeant` (with ngspice: the one pulse), `anEquationNamedAsANetIsWarned`, `aDialogsTreeIsReadAndChecked`, `diagramsGoOnTheDataDisplay`, `unnamedPinsOntoNamedOnesAreGuessed` (the OpAmp turned and not, and a turned resistor onto a subcircuit), `aModuleNotLoadedIsExplained`. Changed: `aStaleDatasetIsSaid`. The copy's move is no longer stale, a value is, certainly. A dataset touched after its run falls to the time, not certainly. `tune` without `apply` and a failed run are certain.
- **`test_osdi_selection`**: `withoutAProjectNoLibraryIsLoaded` tested the gate; it is now `withoutAProjectTheLibrariesBesideItAreLoaded` (beside it, not its subfolders), with `theCheckLooksBesideTheSchematicWithoutAProject`.
- **`test_project_scratch`**, **`test_project_folders`**: see the correction below.
- **`scripts/mcp-e2e-scenarios.py`**: s9 added; s1 to s9, 71 checks, none failed, run on the build tree's app.

Each change was broken on purpose, 40 breaks in all, and each is caught. Three
were not at first:
- Opening a closed `.dpl` could be broken unseen: with its schematic open, the
  other path made the data display anew, which opens the one there. The test
  now closes the schematic too.
- Reading a part's sides as it was placed, not as its type draws it, could be
  broken unseen: the OpAmp's roles decided before the sides did. The test now
  replaces a resistor turned round, which has no roles, with a subcircuit
  whose pins are a on the left and b on the right; by number is right.
- The break of the netlist a run keeps did not compile at first. Rerun with
  the statement emptied, it is caught.

The full suite passes (79). `~/QucsWorkspace` and `~/Library/Caches` are
as they were before the final runs (checked against a marker file).

## A correction: the tests' caches

The first full run of the suite with this change wrote each run's kept
netlist into `~/Library/Caches/test_*/netlists`. That is not the
user's `qucs-s` cache, but it is the user's Library. Those folders were
removed (seven of them had been made by that run, `test_qucs_control`'s
`projects` from 28 September is left). `misc::cacheDir()` now honours
`QUCS_CACHE_DIR`, and the tests' `useIsolatedSettings` sets it inside
their temporary folder. `test_project_scratch` checks that it does.
Two tests had compared with the system's cache location; they now
compare with `misc::cacheDir()`.

The tools sent every turn grow by one, `set_subcircuit_parameters`, which
is found by the tool search rather than loaded every turn.
