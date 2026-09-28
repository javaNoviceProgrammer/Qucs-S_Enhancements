# Feature gaps: Qucs-S's MCP tools, the second round of feedback

*28 September 2026 — Qucs-S 26.1.4.*

After the first round (`2026-09-28-claude-mcp-api-feedback.md`), Claude
(Fable 5.1) used the tools again on scratch schematics with ngspice 46
and wrote a second report (`qucs-mcp-api-feedback-round2.md`). Part 1
checks the first round: seven points confirmed fixed and five still open.
Part 2 covers the tools the first round had not used: one real bug, three
silent dead ends and some polish. Each point was checked against the
source; the tables say what was done.

## Still open from the first round

| Feedback | Now |
|---|---|
| **Ground refs are one-tool-only.** `connect` takes `GND#2.1`; `move` and `delete` answer "There is no component GND#2". | Every tool that takes a part's name now takes a ground's ref, as `get_schematic` gives it: `GND` when there is one, `GND#2` the second of several. That covers `move`, `delete`, `select`, `edit_component`, `create_subcircuit` and `get_schematic`'s `components`, besides `connect`. A plain `GND` among several is refused with the refs to choose from, and `GND#5` among three says there are three. `edit_component` can turn, mirror and move a ground. Its answer gives the ref, which may change, since a ground put down again comes last. A ground cannot be renamed. `delete` takes a net label's name before a ground's ref, so a label named `GND` is deleted as before. `move` and `create_subcircuit` tell grounds by their refs too. |
| **Disk edits are attributed to the user.** A `sed` of the agent's came back as "changed by the user". | A document loaded again because its file changed on disk has a new author: its file. `get_state` and "Since your last call" now read "changed by its file, changed on disk (by a command such as your Bash, another program or the user) and loaded again". Who wrote the file is not known, so it is not guessed. A file Claude's Write or Edit changed in the dock is credited to that conversation, which is then not told of its own edit. Testing it turned up a trap. An open file is watched once a timer sets its watch, every 1.5 s, so a command that changed a file right after it was saved or opened was never seen. A file is now checked when its watch is set, and loaded again if it is newer than its last load or save. |
| **Bad values are applied, not refused.** A typo like `1kk` still lands. | The number check read any letters after the digits as a unit, so `1kk` passed as a number without a word, and SPICE would have read `1k`. It now reads the letters as they must be: a scale letter (f p n u m k M G T, meg, mil) and a unit (Ohm, Hz, s, V, A, F, H, S, W, dB, dBm, m, deg ...). A value that starts as a number and has other letters after it is refused, and nothing changes. That holds for `add_component`, `edit_component`, `replace_component` and both forms of `set_schematic`. For the `.sch` text, the schematic is put back after it was read, with its undo steps and whether it was changed. The grammar comes from what is there: all 19,395 numeric values of the 252 shipped schematics and every type's defaults pass. Other odd values are still notes, not refusals: a word, which may be a parameter, `4k7`, and `3,3`, which Xyce's HB takes as a list. |
| **The equations parameter still has all its shapes.** | Its schema now says what it is, a list: of `"name=expression"`, with `{"k": null}` to take one away. The other shapes are no longer described. They are still taken, so nothing written for them breaks. |
| **Filtered `get_schematic` renumbers nets.** A read of two parts called a net `net3` that the full read called `net1`. | The nets are named over every part of the schematic before any are listed, so `net1` is the same net in a read of some parts and of all. Unnamed nets are numbered without a gap (net1, net2 ...); `gnd` used to take a number. Names asked for that match no part come back under `not found`. |

## One real bug

| Feedback | Now |
|---|---|
| **`create_subcircuit` fails on a label-only net, and its preview writes to disk.** "The subcircuit written has 0 pins where 1 were meant … (mcp_rc_sub.sch is written)", with `preview` too. | Two bugs, neither of them the label. **The folder:** a schematic learned the folder it is in only when it was loaded from disk. After Save As of a new schematic, files named beside it were not found until it was opened again: subcircuits, SPICE libraries, the component dialog's relative files. The subcircuit's file was there, but not found, so its symbol had no pins. Save As now tells the schematic its folder (`Schematic::setName`). This was a fault of Qucs-S itself, not only of the tool. The feedback's schematic had been saved with `as`, which is why it failed; the label-only net was a coincidence. **The preview:** a preview put the open schematics back, not files. `create_subcircuit` now keeps what its file held, and a preview puts it back after: the file is removed, or given its old content when it replaced one. The preview says which "files it would write". When the tool fails after writing, it puts the file back as well, and no longer says "is written". A previewed `batch` could also run `save_document`, `simulate` or `export_image` and keep what they did. It now holds only calls that change schematics or only look, and is refused otherwise before anything runs. |

## Silent dead ends

| Feedback | Now |
|---|---|
| **`add_analysis` accepts an expression in `plot` and makes a trace with no data.** `db(v(out))` became `ngspice/ac.db(v(out))`. | An item of `plot` that is not a node, `v(...)`, `i(...)` or a dataset name is an expression. `add_analysis` puts a NutmegEq beside the analysis that computes it (`db_v_out=db(v(out))`), named unlike any net or equation there. The trace shows that variable, `ngspice/ac.db_v_out`. On an AC plot it goes on the right axis, in its own units, since the left axis shows dB already. The answer lists the equations and counts the block as a step to undo. Under a simulator with no Nutmeg (Qucsator, Xyce), such a call is refused before anything is added, with the way to do it (an Eqn and its name). Checked with ngspice: `db(v(out))`, `ph(v(out))` and a transient's `v(out)*1000` all had data after one run. Two faults underneath were fixed: a trace named `ac.db_v_out` before any dataset was guessed to be `ac.v(ac.db_v_out)`, and `add_diagram` said "There is no dataset yet" once per trace. |
| **`simulate` refuses an untitled document**, and in a batch stopped the run after nine edits. | `simulate` and `tune` save an untitled schematic in the scratch folder of the project (or of the workspace), as `untitled.sch`, `untitled-2.sch` ... Its netlist and dataset go beside it, and the answer says where it is and that `save_document` with `as` puts it elsewhere. `new_document` says so as it makes one. |
| **`import_netlist` leaves an empty untitled tab behind.** | That tab was the untitled schematic Qucs-S opens at start: `import_netlist` makes a document of its own. An untitled schematic nothing was ever done in is now closed when `import_netlist`, `new_document` or `open_document` makes or opens another. One with anything in it stays. There is one "untitled" to name, where `new_document` used to leave two. |

## Schema polish

| Feedback | Now |
|---|---|
| **Deferred tools carry fewer field descriptions than the core ones.** | 283 of the 359 fields had no description of their own, core tools' included. Every field now has one: what it is, its unit, its default, and the shape of an object such as `tune`'s `measure`. `theToolsAreListed` fails for any field without one. `preview` now says a file it writes is put back too. |
| **`run_script` returns the last expression, not a return value.** A top-level `return` is a syntax error. | A script with a `return` outside a function is run as a function's body, so `return` gives the result. The last expression still does, and error line numbers are still the script's own. The field's description says both, and that `format: 'overview'` gives `components` as a count. |
| **`replace_component` reports changes by property index** ("property 3 neutral →"). | A part of another type is now told as "C1: now type C (was L), C=10m": the new type named, its first value given, and properties both types have compared by name, not by place. This is `describeChanges`, so the preview, "Since your last call", `diff` and `undo_history` all read so. |
| **Wire and pin coordinates are both bare arrays.** | Left as they are. The key says which is which, and the JSON form is what `set_schematic` takes back, so changing it would break that round trip. |

## Tests

- **`test_qucs_control`:**
  - `groundsAreToldByTheirRefs`: `move`, `select`, `edit_component` and `delete` by ref. A plain `GND` among three is refused, and so are `GND#5` and a rename. A filtered read's nets match the full read's, a name matching nothing is under `not found`, and nets are numbered without a gap.
  - `aSubcircuitBesideANewSchematicHasItsPins`: a schematic saved under a new folder, with a label-only net. A preview leaves no file, and one it would replace keeps its content. A previewed batch with `save_document` is refused. The subcircuit has its one port.
  - `aNumberMistypedIsRefused`: `1kk` refused by `add_component`, `10uu` by `edit_component`, `1nn` by `replace_component`, and by both forms of `set_schematic`. After the `.sch` text, the schematic, whether it is changed, and its undo steps are all as they were. Nine good forms are taken, from `4.7 kOhm` to `{Rload*2}`.
  - `anExpressionIsPlottedByAnEquation`: the NutmegEq, its equation, the trace on the right axis, three steps to undo, and the note said once. Under Qucsator the call is refused and nothing added.
  - `anUntitledSchematicIsSimulatedFromScratch`: `simulate` and `tune` on untitled schematics.
  - `anUntouchedUntitledTabIsClosed`, `aScriptReturnsItsResult` (also the line of an error, wrapped or not), `aChangeOfTypeIsToldByName`.
  - `anEditOnDiskIsTheFilesNotTheUsers`: a file changed 50 ms after its save, before its watch is set, is loaded again, and credited to its file.
  - `theToolsAreListed`: every field has a description.
- **`test_claude_code`:** `theApplicationWorksWithTheDock` credits a reload with no conversation named to the file, and one a conversation's turn reports to that conversation.

Each change was broken on purpose, one at a time: 28 breaks, and every
one fails its test. The full suite passes (75), under ASan and UBSan too.
The feedback's scenarios were also run again against the headless server
with ngspice 46: the label-only subcircuit, the expression plots and the
`sed` edit.
