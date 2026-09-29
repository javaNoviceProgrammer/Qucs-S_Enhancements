# Feature gaps: Qucs-S's MCP tools, the third round of feedback

*28 September 2026 — Qucs-S 26.1.4.*

After the second round (`2026-09-28-claude-mcp-api-feedback-round2.md`),
Claude (Fable 5.1) read the tools' source and tested them again, in the
window and against `qucs-s --mcp-server`, at commit `a9912b5`. The report
(`qucs-mcp-api-feedback-round3.md`) confirms the second round's ten fixes
and headless mode. It reports seven bugs, a measurement that misses, and
a quadratic `arrange`, followed by code notes and small things. Each point
was checked against the source and reproduced before it was changed. The
tables say what was done.

## Bugs

| Feedback | Now |
|---|---|
| **3.1 `tune` corrupts a value with no unit or scale letter.** R2 = `330` came out as `10k 330`. The search stopped after two runs and applied the closest value anyway. | Two bugs. **The number as its own unit:** `misc::str2num` read the text after a number's digits as its unit. With no letters, `indexOf` gave -1, so the unit was the whole text: `330` read as 330 with the unit "330", and `tune` wrote that after every value it tried. This is a fault of Qucs-S itself: `valuereading.cpp` worked around it, and `tune` did not. A number alone now has no unit. **A miss applied:** the search had stopped because `100 330` and `10k 330` gave nonsense. It said so, but with `apply` it still set the closest value. A value is now set only when it gives the target within the tolerance. Otherwise the part keeps its value, and the answer gives the closest value and how far off it is. All four rows of the report's table now find `1k` in three runs. The value found is also given in six figures at most: `1k`, not `999.999719`. |
| **3.2 `get_dataset` reads through the DataSet property; `simulate` writes by the file name.** On a schematic copied in Finder, `get_dataset` returned the original's results. | The SPICE runner (`SimulationRun`) wrote `name.dat.ngspice` after the schematic's file name. Everything else followed the schematic's `DataSet` property: Qucsator's runs, the diagrams, the data display and the tools. In the window too, a copy's runs went where nothing read them. The runner now writes where `DataSet` says, as Qucsator does, and `simulate` looks for the dataset there. There is one resolver. The window asks about a copy's names when it opens it, but the tools open files without that box, so nothing was said. `open_document` now tells when a schematic's dataset and data display are named after another file, and which file its runs would write. `own_data_names` names them after the schematic, as the window offers. |
| **3.3 Two ground-naming schemes.** `get_netlist`'s map numbered grounds `GND`, `GND#1`; the other tools use `GND#1`, `GND#2`. | The map now uses `refOf`, which all the other tools use (moved to `qucscontrol_p.h` so both files share it). `said()` in the net-change messages drops the number rather than using another one, so it is vague but not wrong. |
| **3.4 `create_subcircuit` leaves an orphan ground.** | Confirmed on the report's circuit: the ground at 350, 300 was reported "connected to nothing". Orphan grounds and dangling wires are now pruned in turns until neither pass removes anything. A ground that met the group through a wire stub goes when the stub does. |
| **3.5 The documented subcircuit path breaks the parent.** `create_subcircuit`, then `make_symbol` and save, left the instance's pin off its net, and the next netlist read `XSUB1 _net0`. | `create_subcircuit` joins each instance pin to its net by a label on the pin alone. When the symbol changed, the pin moved away. Nothing else was on the node it left, so the node was deleted along with its label. Saving a subcircuit now carries such a label to the pin's new position, one undo step in the parent, and says so ("Their labels followed their pins"). A pin that met a real wire still gets the warning: a wire cannot be moved along. `create_subcircuit` also lists the named nets that go inside and nothing outside is on, with the traces and equations that name them (`gain=db(v(out)) in NutmegEq1`). Those show nothing after the next run. |
| **3.6 `get_dataset` has no staleness check.** | Each document now records how its last simulation ended, in the window or through the tools, and whether it failed. `get_dataset` answers with a `stale` line when the last run failed after the dataset was written. It also does when the netlist a run would be given now differs from the one that wrote the dataset, which is the exact test for "the circuit changed". A moved part or a new diagram changes no netlist, so it is not called stale. Where no netlist can be compared (Qucsator, or another simulator than the settings'), an edit after the dataset is reported as a possible reason. |
| **3.7 Unknown top-level arguments are silently ignored.** A text box's `tip` came back with width 80 instead of 180. | `call()` now refuses an argument the tool's schema lacks. It names the arguments the tool does take, and the one probably meant (`rotaton`: "Meant rotation?"). Nothing is done. A pinned conversation still names its schematic to `get_state`. The painting tools read their type-specific fields from each type's own list and already refused a field the type lacks, so their schemas are marked open (`additionalProperties`) and the generic check leaves them alone. `tip` was not the cause of the width: a new text box given `width` without `height` was sized as a click places one, 80 × 50. The size it was given is now kept. A `tip` given without `pointer` also turns the pointer on: it had been stored and drawn nothing. |

## Measurement

| Feedback | Now |
|---|---|
| **`frequency` on a decaying ring**: 5338 Hz where theory gives 4970. It counted crossings of 0.8, the curve's middle. | Halfway between a ring's extremes is pulled toward its first swing. A damped sine crosses the value it settles at once every half period. A curve whose last quarter swings less than half of its whole swing is now measured at its final value; a steady wave still uses the midpoint. The answer says which level it used (`level is`). Crossings count only after the curve goes 1% of its swing beyond the level, so the still end of a ring does not add false periods. A synthetic RLC ring with ζ = 0.158 measures within 0.2% of the damped frequency; with a 37 MHz ripple riding on it, within 0.5%. `period` changes the same way. |

## Performance at 2004 parts

| Feedback | Now |
|---|---|
| **`arrange` is still superlinear on a long chain**: 6.8 s at 1501 parts, blocking the window. | Measured on RLC ladders: 0.69 s at 335 parts, 2.7 s at 667, 10.6 s at 1335, 23.7 s at 2003. Quadratic. A profile put 5384 of 5384 samples in `placeGround`, one call per ground (500 on the ladder). Each call measured the text boxes of all 2000 parts, scanned every node and wire, worked out every net, and took a text snapshot to restore on failure. A `GroundRoom` now indexes the parts' symbol and text boxes by place, each measured once, together with the nodes and wires. A ground is placed only where nothing is in its way, so nothing needs restoring. The labels placed before the grounds are checked against a cell index too, and the nodes are found by place (`IndexedInsertion`). Times now: 0.06 s, 0.11 s, 0.23 s and 0.33 s. At 2003 parts every net is as it was, the netlist's device connections are the same, and `check_schematic` finds nothing. The 60-stage ladder comes out exactly as the previous build laid it out. |
| **A full `get_schematic` is about 45k tokens; it should cap itself.** | Lists were already capped at 200, but not net labels: 2501 of them, 98 KB. Labels are now capped with the other lists. A read of a schematic of more than 200 parts that names no parts lists the first 50 of each kind, and says to use `region`, `components` or `overview`. The 2003-part ladder's read went from 236 KB to 40 KB. A filtered read now also lists only the labels on its parts' pins or in its region. Before, it listed all of them. |

Testing the examples before and after found a pre-existing fault: `arrange` does not lay a schematic out the same way twice. Two runs of the previous build differed on 58 of the 251 examples, because `arrange` iterates hashes and their order changes from run to run. The sorting added here breaks ties in a fixed order where it matters most: the pieces of a net and their places, a group's members, its sources, and where labels and grounds go. Differences remain, from hashes keyed on pointers, whose values change with every run. That is left for a later round. Across the examples the new wiring is about 8% shorter, and no example's got notably longer.

## Code-level notes

| Note | Now |
|---|---|
| **Eight per-tool tables and one if-chain; nothing checks they agree.** | `everyListedToolIsDispatched` calls every listed tool with `{}` and fails on "There is no tool". Unknown arguments are now checked against the same schemas. |
| **The script watchdog's timeout is approximate.** | Said in `run_script`'s description: a call running when the time runs out, such as a simulation, finishes first. |
| **Preview cost.** | Unchanged. Measured: `arrange` with `preview` on the 2003-part ladder takes 0.33 s. |
| **`add_analysis` follows the settings' simulator, `simulate` takes a per-run one.** | `add_analysis` takes `simulator` too: `ngspice` makes its expressions a NutmegEq under a Qucsator setting. The refusal says so. |
| **`describe_tool`'s summary equals the full description for non-core tools.** | Every tool's summary is kept and listed. |
| **Paths: any tool reads and writes anywhere.** | One sentence in the README's headless section: give `--mcp-server` only to clients you would let use your files. |
| **Payload of `tools/list`.** | Unchanged: the core/search split keeps the per-turn cost down. |

## Small things

| Feedback | Now |
|---|---|
| **`arrange` on an imported netlist leaves label-joined nets as stubs.** | `wire_labels: true` keeps each net's label on its first piece only, and the wiring pass joins the other pieces with wires. The 500-stage ladder gets 2499 wires in 0.24 s, every net as it was. |
| **`diff` after undoing to step 0 answers "'steps' is 1 to 0".** | It now says there is no step to go back to, and that `diff` without `steps` compares with the file. |
| **`move` with dx = dy = 0 is an error.** | It moves nothing and says what would move: the parts, the wires that go with them, and the wires to the rest of the circuit that would be drawn on to them. |
| **`undo`'s "Undone. Now: R1: turned" reads as the current state.** | It now reads "The undo changed: …" ("The redo changed: …"). |
| **Filtered `get_schematic` renumbers unnamed nets.** | Round 2's fix holds: 206 filtered reads of example schematics against their full reads, and no pin's net differs. |
| **`open_document` gives no warning about DataSet or DataDisplay.** | See 3.2. |

## Tests

- **`test_qucs_control`:**
  - `aPartIsTunedToATarget` gains two checks. A value that is a number alone (`330`) is tuned without its number taken for a unit, every value in six figures. A missed target is not set.
  - `aCopyMadeOutsideQucsIsToldOfItsDataNames`: the note on opening, the run writing and `get_dataset` reading the same file (with ngspice), `own_data_names`, and the run afterwards writing the copy's own dataset.
  - `theSubcircuitPathKeepsTheParentWhole`: the map's ground refs against `get_schematic`'s, no orphan ground, the "inside now" note naming the NutmegEq, and the label following the pin after `make_symbol` and a save. The parent then netlists `XLP1 lp_n1`.
  - `aStaleDatasetIsSaid`: not stale after a diagram or a move; stale after a value changed, fresh after a run, stale after a failed run.
  - `unknownArgumentsAreRefused`: `rotaton` refused with "Meant rotation?"; `preview` on a tool that takes none; an unknown argument inside `batch`; a text box's width alone kept; a tip turning the pointer on; a field a text box lacks refused by the painting tool.
  - `aLargeSchematicIsArrangedInTime` gains a ladder case: 400 stages must take less than eight times 100 stages. The whole read is 50 parts and at most 50 labels, and a filtered read lists only its part's label.
  - `anImportedNetlistIsWiredWhenAsked`, `theThirdRoundsSmallThings` (`diff` at step 0, a zero `move`, the undo wording, `add_analysis` with its own simulator), and `everyListedToolIsDispatched` (with `describe_tool`'s summaries).
  - Updated: `aSummaryIsAsLongAsAsked` (50 parts from a whole read, 200 from a region) and `aGroupMovesWithItsWires` (a sub-grid move is a no-op, not an error).
- **`test_dataset`:** `curvesAreMeasured` measures a ring's damped frequency, with and without a ripple.
- **`test_value_reading`:** `aNumberAloneHasNoUnit`, for `misc::str2num`.

Each change was broken on purpose, one at a time: 28 breaks, and every
one fails its test. The full suite passes (75), under ASan and UBSan too.
The report's scenarios were run again against the headless server with
ngspice 46: the `tune` table, the copied schematic, the subcircuit path,
the text box, and the 2003-part ladder. The fuzzers of the 2026-09-28
hunt (`docs/bug_hunts/2026-09-28-claude-tools/`) ran 16,000 calls on the
ASan build, half of them pipelined: no crash, no hang, no sanitizer
report.
