# Feature gaps: Qucs-S's MCP tools, the fourth round of feedback

*28 September 2026 — Qucs-S 26.1.4.*

After the third round (`2026-09-28-claude-mcp-api-feedback-round3.md`),
Claude (Fable 5.1) tested the tools again at commit `f0d20e3`, in the
window and against `qucs-s --mcp-server`, while a second Claude read the
diffs of `d8f651e` and `f0d20e3`. The report
(`qucs-mcp-api-feedback-round4.md`) confirms most of the third round. It
reports four bugs, two schema regressions that the unknown-argument check
caused, and a list of smaller things and code notes. Each point was
checked against the source and reproduced before it was changed. The
tables say what was done.

## Bugs

| Feedback | Now |
|---|---|
| **2.1 The subcircuit path still breaks the parent when a wire ends on the pin.** `create_subcircuit` puts the instance where the parts were, so its pins land on the ends of the wires that reached them. After `make_symbol` and a save, the pin moved and the parent netlisted `XSUB1 _net0 out lp`. | Reproduced with the report's circuit: `R1` lying between the source's wire and the capacitor's. The third round carried only a label on a pin alone to the pin's new place. Now a pin that leaves a wire end gets a wire drawn from where it was to where it is. The router is the one `connect` uses: it goes over no other pin or wire and joins those two nets and nothing else. Labels still follow as before. The answer lists the pins that moved and what joined them again (one undo step in the parent). Then each parent's nets are compared with how they were before the save, pin by pin. A pin that comes to rest on another net's wire, or one no repair could reach, is named ("the nets of usestwopin.sch are not as they were - X1.1 is on the net of R2.1"). If every net is intact, the answer says so. The report's case now netlists `XSUB1 in out lpw`, and `check_schematic` finds nothing. |
| **2.2 A preview that changes nothing counts as an edit, and the dataset is then called stale.** | A preview put the schematic's elements and undo stacks back, but not its revision or its record of edits. Each schematic's revision and recent edits are now kept before a preview and put back after it. A preview is invisible to `get_state`, to "changed since your last call" and to `get_dataset`'s staleness test. While a previewed `batch` runs over several turns of the event loop, a subscriber to the schematic's resource reads the revision from before the preview. When no netlist of the dataset's run is at hand (a schematic copied, renamed or opened and not simulated here), the `stale` line now says that only the time of the edit tells. |
| **2.3 Unknown keys inside a `set_schematic` component object are ignored.** `rotaton` placed R9 unrotated. | The unknown-argument check now walks the schema. Nested objects are checked too: the items of an array and an object argument's fields. It is refused as at the top level ("set_schematic: components[0] (R9) has no rotaton; it takes ... Meant rotation?"), and nothing is done. `set_schematic`'s part and wire now list their fields in the schema, as `get_schematic`'s JSON form gives them, and that form is still taken back whole. `add_diagram`'s trace objects and its axes, `delete`'s traces and `batch`'s calls are checked the same way. The name suggestion also catches swapped or missing letters now (`lable`: "Meant label?"; `colour`: "Meant color?"). |
| **2.4 The fuzzer wrote into the user's real workspace.** 105 entries in `~/QucsWorkspace` (`"`, `{}`, `<Components>_prj`, ...). | The fuzzer set `QUCS_SETTINGS_DIR` to an empty folder, so `main.cpp` fell back to `~/QucsWorkspace` and created it. Three changes. **`--workspace DIR`** gives a run its own workspace, read before the settings create theirs, with its scratch folder (`spice4qucs`) inside it. It is not saved as the settings' workspace, nor as the file browser's last folder. **The hunt's scripts** (`fuzz_tools.py`, `fuzz_pipe.py`, `mcp.py`, and the smaller drivers) now pass `--workspace` with a folder of their own (one per fuzzer seed). The fuzzers and `mcp.py` stop before the first call if `get_state` reports another workspace. A `.gitignore` there keeps their output out of the repository. **Names that are no file's name** are refused by `new_project`, `save_document`, `copy_document`, `create_subcircuit`, `export_netlist` and `export_image`: a character Windows refuses (`<>:"/\|?*`, control characters), a leading space or a trailing space or dot, a Windows device name (`NUL`, `COM1`), or no letter or digit before the suffix (`'`, `{}`). The name is checked as given, before the path is cleaned: the fuzzers, run again, found `save_document` with `as: "."` writing `<workspace>.sch` beside the workspace (`.` is the workspace, and `.sch` went after it). `.` and `..` are refused as folders. The junk moved to `~/Library/Caches/qucs-s/fuzz-junk-2026-09-28/` is the user's to delete. |

## Schema regressions

| Feedback | Now |
|---|---|
| **5.1 `replace_component` lost `records`, `specs` and `replace_equations`.** | Added to its schema. Swapping one pinless block for another (a Monte Carlo for corners, or for another Monte Carlo with new records) had been refused ("has no pins: nothing would take MC1's nets"). The new block now goes where the old one was, or at `x`, `y`. |
| **5.1 `redo` lost `to`.** | Added (and described), as `undo` has it. |
| **A test that diffs each handler's keys against its schema.** | `scripts/ci/check-tool-arguments.py` does this and runs first in the Linux CI job. It reads the schemas, `call()`'s dispatch, and every key each handler reads: `args.value("k")`, `contains`, a lambda's or a table's keys. It follows `args` into the functions it is handed to, and aliases such as `get_netlist`'s copy that drops `save_as`. It reports keys read but not in the schema (refused), and schema keys nothing reads (silently ignored). It also checks nested item schemas against the functions that read them (`componentLineOf`, `wireLineOf`, `addDiagram`'s traces, `BatchRun::next`, `delete`'s traces, `set_dialog`'s controls). On `f0d20e3` it reports exactly the four regressions above; now it reports none across the 67 tools. |

## Smaller things

| Feedback | Now |
|---|---|
| **`tune` interpolates in log(value) even when the response is linear.** Six runs for mid = U/3. | The search starts linear, or logarithmic when the range spans two decades or more. After each run it compares what the linear and logarithmic secants between the bracket's ends would have predicted, using the ends as measured (not Illinois' halved values), and keeps the closer one. The report's case, and a divider's U for 1.5 V, now take three runs. Worked out beforehand on six responses, it takes the fewer runs of the two, or one more. A linear response over three decades (begun on the logarithm): 4 runs, 10 staying on the logarithm. A divider's R2 over 100 Ω to 9 kΩ (begun linear): 6 runs, 8 staying linear. dB over a resistance: 4, against 8 linear and 3 logarithmic. |
| **`tune`'s `values` rewrites the value text** (`10 Ohm` as `10`, `.5k` as `500`). | A value of the list is set, reported and applied as it is written there (`3 kOhm`). Values the search finds are still written in six figures. |
| **The netlist map calls the ground node `gnd`; the netlist writes `0`.** | The map's `nodes` call it `0`, as the netlist and the map's own note do. |
| **`simulate` runs despite its pre-run check finding a pin connected to nothing; the cause is buried.** | The check's findings are now under `before the run` (the answer's fields are sorted by name, so it comes first). When the run fails, the check's errors and its warnings about a part come first in `errors`, marked "before the run, by Check Schematic". Loose wire ends stay out of `errors`. |

## Code reading (5.2)

| Note | Now |
|---|---|
| **`followPins` matches by pin index, not port name.** A port inserted or renumbered shifts every index. | Pins are matched by name when every pin had a name before and has one now, each name once. Otherwise they are matched by number: a symbol drawn for the first time replaces nameless default pins. The net comparison uses the same mapping. A label whose pin moved while another pin of the same instance now sits where it was goes with its own pin. A wire is never drawn on to a place where another pin of the instance sits: the comparison reports that case instead. `aRenumberedPortKeepsItsNet` adds a port numbered 1 ahead of `a` and `b`, draws the symbol again, and checks each label follows its port by name. |
| **`staleness()` writes a whole netlist on every `get_dataset`.** | Only when something could have changed since the run began: an edit after the run's netlist was written, or the schematic's file written after it (by another program, before it was opened). `tune` edits before each run, so its reads no longer netlist. The preview fix keeps previews out of this. |
| **`arrange`'s `byPlace` is not a total order.** | The part's index is the last key, as the other sorts have. |
| **`valueText()` gives "nan" below about 1e-303.** | The rounding is skipped when its scale is not finite. |
| **Filtered `get_schematic` leaves out labels off the wanted parts' pins.** | One clause in the description: a label elsewhere on their nets is left out, and its name is in `nets`. |
| **Nits.** | The stale comment in `valuereading.cpp` is corrected. `open_document`'s copy notice compares the data names without case where the file system ignores case (probed: the schematic found under its name with the case of every letter flipped). `Foo.sch` with `foo.dat` is its own there. |

## Tests

- **`test_qucs_control`:**
  - `theFourthRoundsArguments`: `replace_component` with records and specs on a Monte Carlo block; `redo` with `to`; unknown keys inside a `set_schematic` part, a wire, an `add_diagram` trace and axis, and a `delete` trace, each refused with the name meant; `get_schematic`'s JSON form taken back whole; six names that are no file's refused by `save_document`, `new_project` and `export_netlist`.
  - `aPreviewIsNoEdit`: revision and edits unchanged after a preview that changes nothing, one that would add a part, and a previewed `batch`; the resource's version as before.
  - `aSubcircuitPinOnAWireIsWiredOn`: the report's circuit; the moved pin wired on, every net as it was, `check_schematic` clean and `XSUB1 in out` in the netlist.
  - `aRenumberedPortKeepsItsNet`: see 5.2. The new port 1 is put where `a`'s pin was: the label left there is `a`'s, and goes with it.
  - `theFourthRoundsTuneAndSimulate` (ngspice): a linear response found on the third run; the switch both ways (at most 5 runs from a logarithmic start, 6 from a linear one); a value of `values` applied as written; a failed run whose first error is the check's.
  - `aDatasetNamedInAnotherCaseIsItsOwn`.
  - Extended: `aSavedSymbolIsTakenByItsInstances` (a wire drawn on; a pin moved onto another net's wire end said), `theSubcircuitPathKeepsTheParentWhole` (the new wording), `aStaleDatasetIsSaid` (a preview after a run is no edit; a copy's edit is told from its time, and that it cannot be compared), `aPinIsConnectedToGround` (ground is `0` in the map), `aSimulationIsWaitedFor` (the check's field renamed).
- **`test_mcp_server`:** `theWorkspaceIsTheOneGiven` runs the program with `--mcp-server --workspace`: `get_state` names that folder, a project named `{}` is refused, `amp` is made there, and the settings file does not name the folder. The program the tests start now gets a `HOME` of its own. With `--workspace` broken on purpose, the fallback went to the real `~/QucsWorkspace`: a break check of this round left an empty `amp_prj` there, since removed. Now it fails the test instead.
- **`scripts/ci/check-tool-arguments.py`** in CI.

The tools sent every turn grew by 691 bytes (29,597 to 30,288, about 170 tokens) for the part and
wire fields in `set_schematic`'s schema.

Each change was broken on purpose, one at a time: 24 breaks, and each one fails its test. Three
were not caught at first: the preview's rewind (on the original schematic the exact netlist
comparison hides it, so the test now previews on the copy), the renumbered pin's label (the new
symbol never put another pin where the old one was, so the test now does), and the switch between
secants (the linear case ends before any switch). The tests were extended until they were. The full
suite passes (75), under ASan and UBSan too. `test_drop_open` missed a 5 s timeout once while eight
sanitized tests ran together, and passes alone. The report's subcircuit path was run again against
the headless server with ngspice 46. The fuzzers of the 2026-09-28 hunt ran 13,800 iterations on the
ASan build, 5,500 of them two calls at a time: no crash, no hang, no sanitizer report. They found the
`.` name, which is fixed. `~/QucsWorkspace` is as it was before the runs.
