# Feature gaps: Qucs-S's MCP tools, feedback from the model's side

*28 September 2026 — Qucs-S 26.1.4.*

Claude (Fable 5.1) used the `mcp__qucs__` tools from the Qucs-S dock and
wrote down what works and what still costs it (`qucs-mcp-api-feedback.md`).
It found the tool set capable and asked for polish: fewer silent mistakes,
fewer round trips, plainer text, some help with layout. Each point was
checked against the source; the tables say what was done.

## Where it has cost

| Feedback | Now |
|---|---|
| **The raw escape hatch is unchecked.** `set_schematic`'s `.sch` text loads property values in order, with no count check: silent mistakes. | The count was already checked: a line with a value too many is refused, and one with too few is taken and noted (round 5). What was missing was a check of the values themselves: a line with the right count but a value left out and one added, or two swapped, loaded silently. The text path now runs the same check as the JSON form on every part it read and lists what does not fit under `values`. A number is expected where the type's default is a number. For a word, it checks that an equation block in the schematic defines it as a parameter; the note says this is fine for a subcircuit's parameter or a name from an included file. A value must also be one of the property's choices when its description lists them (`[european, US]`, `[lin, log, list, const]`, `[yes, no]`; only lists of two words or more). The note ends by pointing at `describe_component_type` and at the JSON form. The word check now also covers `add_component`, `edit_component` and the JSON form. |
| **Some typed tools lagged the format** (`.OPTIONS` flags, Monte Carlo records). | Closed in round 6, as the feedback says: `flags`, `records` and `specs`. |
| **Bash edits are invisible to the editor.** | They are not, since the file watcher of 2026-09-27: a file changed on disk is loaded again when its document has no unsaved changes. Nothing told Claude when it was not, though; that is the reload trap below. |
| **The prose style** is dense and idiosyncratic. | Every tool description was rewritten in plain English: 64 descriptions, `run_script`'s and `describe_tool`'s, the 66 summaries and the server's instructions. Each now starts with a verb and says what the tool does, then its arguments, defaults and what it returns. Every fact, default and trap was kept, and the new behaviour of this round was added where it belongs. |

## Smaller things

| Feedback | Now |
|---|---|
| **Layout is entirely on me: some "arrange" help.** | **`arrange`** lays a whole schematic out again for a person to read, as one undo step with a `preview`. Parts go in columns by signal flow. The sources start, each part goes one column right of the part that drives it, and ground and rails (a DC source's net with three parts or more, or any net of more than five) are not followed. A DC supply gets a column of its own at the left; with no signal source, the flow starts from what the supply feeds. Two-pin parts are turned the usual way: in series lying down with the driving side on the left, and to ground or a supply standing up with ground below and the supply above. Down each column, a part sits where its pin lines up with the pin it joins to its left, so a chain runs straight. Every wire is then drawn again by the router `connect` uses, around the parts and never over a pin. Each piece of circuit that had a ground symbol gets one back (on the pin when it fits), and each net label goes back on the pieces that carried it. Blocks without pins go in a row below, and diagrams and paintings the circuit would cover move to its right. Every net is compared before and after (the ground symbols' own pins aside, by the symbols, not their names). If one would differ, or the router finds no way, it tries more room and then the parts' own orientation, and if nothing works the schematic is left as it was and the answer says why. A label on wires that reach no pin, of a name no pin's net has, joins nothing: it is dropped and said. On 150 of the shipped examples, 149 were arranged with every net as it was; the one refused has no parts with pins. A carefully drawn schematic may read better as it was, as the description says. |
| **The equations parameter has too many shapes.** | One form is documented first: a list of `"name=expression"` in order, as `get_schematic` gives it. `edit_component` changes those it names; `{"k": null}` in the list takes k away (such an item was not accepted before); flags go in `flags`. The other shapes are still accepted and mentioned second. The answers of `add_component` and `edit_component` for an equation block now list its equations as they are afterwards (`equations`), so what came of what was given is seen at once. |
| **Two-level docs cost a round trip.** | Only the 22 tools loaded in every turn have short descriptions. The others are found by Claude Code's tool search when a task needs them, and load then with their full description, so they need no `describe_tool` before their first use. In every turn: 22 tools, about 5.6k tokens with their schemas; the other 44 about 13k, paid only for those loaded. |
| **Ground naming** (`GND#2.1`). | `connect` takes `"ground"` (or `"gnd"`) at either end. The pin gets a ground symbol of its own: on the pin when the symbol fits there (under a part's lower pin), otherwise a little away and wired to it. A place clear of the other parts' texts is tried first, then one clear of their symbols. A pin already on ground is said, and nothing is drawn. `GND#2.1` still works. |
| **The reload policy is a quiet trap.** | When a file changed on disk is not loaded again because its document has unsaved changes, every conversation's next Qucs-S tool result says so under "Since your last call". This covers Claude's Write and Edit (through the dock) and Bash or another program (through the file watcher). The note reads: the window shows the unsaved changes, not the file; a save there asks before writing over the file; change it with the tools, or ask the user to save or discard theirs. It is told once, and for half an hour at most to a conversation that comes later. The dock's own note says the same to the user. The instructions now say when a file edit is reloaded. |

## Also found

- **A netlist that could not be written said nothing.** Found while
  checking `arrange` on the examples: the first `get_netlist` of some
  schematics gave a title line alone. The netlister had given up, and its
  reason went to a text nobody read. `get_netlist` now answers
  "The netlist could not be written: ERROR: "D3": Cannot load library
  component …".
- **The second netlist then left a part out, without a word.** The
  netlister remembers each subcircuit and library it has taken in. Taken
  in before it gave up, one was remembered, so the next netlist skipped it
  as already written: a netlist without the part, and no error. What it
  took in is now forgotten when it gives up, so the next netlist fails
  again with the reason. Simulations go through the same path.
- A development build has no installed libraries, so example parts from
  them fail as above; the disk image has them.

## Tests

- **`test_qucs_control`:**
  - `textValuesAreCheckedToo`: a value left out, two swapped, and a correct line with a parameter's name.
  - `equationsAreEchoed`: the list form, `{"k": null}`, flags, and nothing for a part that is not an equation block.
  - `aPinIsConnectedToGround`: on the pin, wired beside it, already on ground, both ends ground refused.
  - `aSchematicIsArranged`: a messy circuit built by the tools, with a label and a dangling labelled wire. Checked: the preview changes nothing; the columns, grounds and labels; the dropped label; every net; one undo step; pins on the grid; no symbols over each other. A series part lies with its driving side left, a shunt part stands with ground below, and the chain C2 to R3 runs straight. Undo gives the schematic back. Xyce's RCBlock, whose subcircuit port is named GND, keeps its nets.
  - `aNetlistThatCannotBeWrittenSaysWhy`: a subcircuit that cannot be read is said twice.
- **`test_mcp_server`:**
  - `anEditNotLoadedIsTold`: told once.
  - `theToolsAreAnnotatedAndTiered`: a tool not in every turn carries its full description, the one `describe_tool` gives; `arrange` takes `preview`.
  - The case of a word as a value now defines the parameter first.

Each change was broken on purpose, one at a time, and 13 of the 14 breaks
fail their test. The one that does not is the last net comparison in
`arrange`, a backstop: the rewiring already refuses a way that joins two
nets, and succeeds only when no net is in pieces. The full suite passes
(75), under ASan and UBSan too.
