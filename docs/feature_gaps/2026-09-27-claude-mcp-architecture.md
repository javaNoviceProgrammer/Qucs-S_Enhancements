# Feature gaps: Qucs-S's MCP server, architecture

*27 September 2026 — Qucs-S 26.1.3.*

A Claude Code session in the Qucs-S dock wrote down how the Qucs-S tools
look from the model's side, and fifteen suggestions for them
(`qucs-s-mcp-suggestions.md`). The transport stays as it is: Qucs-S's own
MCP server, served over Claude Code's stream. The suggestions are about the
tool surface. Its top three were the structured schematic model (6),
destructive annotations (10) and tests against the server (13). Each
suggestion was checked against the source; the tables say what was done.

## Round 1: the tool surface

| Suggestion | Now |
|---|---|
| **1. Tier the tools.** About sixty tools with long descriptions load into every turn, and most sessions use a dozen. | The server as a whole is no longer marked "always load". Each tool carries Claude Code's `_meta` instead: the 22 that most sessions use have `"anthropic/alwaysLoad": true`. They are `get_state`, `get_schematic`, `set_schematic`, `add_component`, `edit_component`, `delete`, `connect`, `add_wire`, `set_label`, `move`, `batch`, `simulate`, `get_dataset`, `check_schematic`, `get_netlist`, `undo`, `screenshot`, `open_document`, `save_document`, `describe_component_type`, `list_component_types` and `describe_tool`. The others have `"anthropic/searchHint"`, words that Claude Code's tool search finds them by (`tune`: "tune optimize a part value to reach a target"). Measured on the tools as listed: all 65 were about 17.5k tokens and are about 9.6k; the 22 were about 8k and are about 4.2k. |
| **2. A scripting tool.** `batch` cannot loop or branch. | **`run_script`** runs a short JavaScript program with Qt's engine (QJSEngine). `qucs.call(tool, args)` calls any tool and returns its answer as an object; a failed call throws with the tool's error. `qucs.log(...)` writes to the result's log, and the program's last value is the result. The answer is `{calls, seconds, log, result}`. A failure is told with its line, and with `"atomic": true` every open schematic is put back as it was, its undo history too. A script is stopped after `timeout` seconds (60 by default, at most 600) or after 5000 calls, and cannot call `run_script` itself. Qt's Qml module is needed: the build option `QUCS_SCRIPTING` (on) uses it when CMake finds it, and says "Scripting (run_script): on/off". Without it, the tool is not listed. The Qt that CI installs on macOS and Linux (aqt) includes it already, and the Windows builds now install `qt6-declarative` from MSYS2. windeployqt is told `--no-quick-import`, since there are no QML files. |
| **3. Resources for read-only state.** | The server offers resources, with `resources/list`, `resources/templates/list`, `resources/read`, `resources/subscribe` and `resources/unsubscribe`. `qucs://state` is the window's state (`get_state`). For each open schematic saved to a file, `qucs://schematic/<path>` is its text as it is now, unsaved changes included, and there is also `qucs://netlist/<path>` and `qucs://netlist-map/<path>`. `qucs://dataset/<path>` exists once a simulation has written the dataset (the path is percent-encoded). An unknown URI is MCP's error −32002. The tools stay: `get_dataset` and `get_netlist` take arguments (measurements, ranges, `last`) that a resource has no place for, and a model in Claude Code reads a resource through a generic tool anyway. |
| **4. Push change notifications.** | A subscribed resource is told of with `notifications/resources/updated` when it changes. For a schematic, that is a change of its revision (an edit by the user, by Claude, or a load from disk). For a dataset, a new write; for the state, a document opened, closed or changed. `notifications/resources/list_changed` goes out when the resources themselves change (a document opened or saved under a name). The server looks once a second. The note of what the user changed since the conversation's last call stays: a notification reaches the client, while the note puts the change in front of the model at its next call. It is now its own field (`since_last_call`) in the structured result. |
| **5. Structured output everywhere.** | Every tool result carries `structuredContent` beside its text. A tool's JSON goes in as it is, a list as `{items}`, a sentence as `{summary}` and an error as `{error}`. A script's error keeps its fields (`line`, `put back`). Notes that followed a result (a document switched back from its symbol, …) are under `notes`. |

## Round 2: robustness, safety, workflow

| Suggestion | Now |
|---|---|
| **6. A structured schematic model.** A `.sch` line's values are positional, so one value too many shifts every value after it. | `set_schematic` takes `components` and `wires` as JSON, in place of `text`. A component is `{type, name?, x, y, rotation?, mirror?, properties: {R: "1k"}, equations?, flags?}`. Each property is checked by name against the type: an unknown one is refused with its place (`Not changed: components[0]: R has no property Rx; its properties are: …`), and so are an unknown type and two parts of one name. Parts without a name are named in turn (R1, R2, …). A wire is `{from: [x, y], to: [x, y], label?}`, straight across or down; a slanted wire is refused. A label alone is `{at: [x, y], label}`. Text and JSON in one call are refused. `get_schematic` with `"format": "json"` gives this form, and `set_schematic` takes it back as it is: the round trip gives the same `.sch` text. Refused, the schematic is left as it was. |
| **7. Warn on bad values.** | A property value that does not read as a number, where the type's default is one, is told in the answer's `values` (`R1: R = "1,5k" does not read as a number (its default is …`). It is kept, since it may be a parameter's name. The notes come with `add_component`, `edit_component` and the JSON `set_schematic`. A type's traps are told as the part is placed (`watch`). A `Vpulse` or `Ipulse` is one pulse (`Vrect` or `Irect` repeats). A `Vdc` or `Idc` gives nothing in AC. An `Eqn` variable named like a net clashes under ngspice. An `OpAmp` clips at `Umax`. A `SpiceOptions` option with no value is a flag. Flag options themselves came in round 6. |
| **8. Watch the file on disk.** | This was there already: the open schematics' and text documents' files are watched. One changed by another program is loaded again when it has no unsaved changes; otherwise the status bar says why it was not. Only text documents were tested; a schematic written by another program is now tested too, along with the notification that follows. |
| **9. Dry run and diff.** | 23 tools that change a schematic take `"preview": true`: `set_schematic`, `add_component`, `edit_component`, `replace_component`, `delete`, `move`, `connect`, `add_wire`, `set_label`, `add_analysis`, `create_subcircuit`, `rename_net`, the painting, diagram, trace and marker tools, `make_symbol` and `batch`. The change is made, described part by part and taken back. The undo history, the unsaved flag and the symbol view are left as they were. The answer is `{preview: true, changed: false, "would change": [...], "its answer": {...}}`, the last being what the tool would have answered. A tool that does not change a schematic says it has no preview. **`diff`** tells what differs, part by part, between a schematic and one of three things: its file as saved (the default), a number of steps back in its undo history (`steps`), or another file or open document (`against`). |
| **10. Scope the dangerous operations.** | Every tool carries MCP's annotations. `readOnlyHint` and `idempotentHint` are set for those that only look. `destructiveHint` is set for every tool that may change or take away what is there, which is all but the looking ones and those that only add (`add_component`, `add_wire`, `new_document`, `simulate`, …). `openWorldHint` is false for all. The host also says which uses cannot be undone, whatever the tool: `clean_scratch`; `close_document` discarding unsaved changes; `copy_document` with `replace`; `export_image`, `export_netlist` or `get_netlist` with `save_as` onto a file that exists; `save_document` as a file that exists; and a `run_script` that may do any of these. The dock asks about such a use every time, even after *Allow Qucs-S Control* or in *Accept Edits*, and its question offers no "allow all". It says "This cannot be undone: …". Only *Auto* and *Bypass* go on without asking. |
| **11. The netlist-to-schematic mapping.** | `get_netlist` with `"map": true` gives `{netlist: [lines], lines: [{line, part, text}], nodes: {node: [pins]}}`. Each netlist line is tied to its part (a continuation line, `+ …`, to the part before it), and each node to the pins on it. It is also the resource `qucs://netlist-map/<path>`. |
| **12. Shorter descriptions.** | Each tool's description is now two or three plain sentences: 6,072 characters for all of them, from 38,019. **`describe_tool`** gives a tool's full description, its input schema and its annotations (without a name, every tool's short one). The server's instructions say that each description is a summary and that `describe_tool` gives the whole of one. |
| **13. Tests against the server.** | **`test_mcp_server`** drives the server by its own protocol (JSON-RPC messages in; answers, notifications and elicitation requests out) on the real application. A last case runs the program itself, `qucs-s --mcp-server`, over stdio. See Tests below. |
| **14. Elicitation for the user's choices.** | The tools ask the user through MCP elicitation, and the dock shows a question as a card. A single yes/no is two buttons, a single choice a button per option, anything else a small form, and there is always Cancel. Questions come one at a time. Three are asked: `save_document` `as` a file that exists, without `replace`; `copy_document` onto a file that exists, without `replace`; and `close_document` with unsaved changes and no `unsaved` (Save, Discard or Keep it open). With no one to ask (a headless client that cannot elicit), the file is not written over and the answer says what `replace` or `unsaved` does. |
| **15. Headless mode.** | **`qucs-s --mcp-server`** serves the same tools over stdin and stdout with no window on screen (Qt's offscreen platform), for any MCP client (`claude mcp add qucs -- qucs-s --mcp-server`). Schematics named after the option are opened. Only the protocol goes to stdout; anything else the program prints goes to stderr. It has an autosave folder of its own and starts no claude or gh. A dialog that would ask at the start (no ngspice found, …) is closed and named on stderr. A dialog a tool opens later is read with `get_dialog` and answered with `set_dialog`. The server ends when its input ends, after the last answer is written; unsaved changes are left unsaved, and stderr says how many. Questions go to the client when it declares elicitation. |

## Also found

- **`diff` against the document's own file** compared the document with
  itself, since the path found the open document: it now reads the file
  as saved. The new test found this.
- **Headless requests out of turn**: each tool call was started a turn
  of the event loop after its line was read. A `resources/list` sent right
  after a `save_document`, without waiting, was answered before the save
  was made. A call now starts as its line is taken, so a request finds the
  changes of those before it, unless one of them goes on in the
  background (a simulation). The disk image's program showed this.
- A test that wrote over `divider.sch` from an earlier case now gives
  `replace`. Without it, the save is refused with the reason, since no one
  can be asked.

## Tests

`test_mcp_server` (new, 10 functions):

- `theToolsAreAnnotatedAndTiered`: every tool has a short description, an
  object schema, annotations and a tier; annotations of the looking, adding
  and taking-away tools; `preview` only on the tools that change a
  schematic; `run_script` listed only where it is built; `describe_tool`
  whole and listing all.
- `resultsAreStructured`: JSON, a sentence and an error as structured
  content.
- `resourcesAreReadAndWatched`: the list (and `list_changed` when a
  document comes); the state, a schematic's text, netlist and map read; an
  unknown URI refused; a subscribed schematic told of after an edit, and
  after another program wrote its file (loaded again); a dataset's
  resource once there is one.
- `theUserIsAskedBeforeAFileIsWrittenOver`: `elicitation/create` sent to
  the client; "no" leaves the file, "yes" writes it; `replace` asks
  nothing; `irreversible()` for a file that exists and one that does not.
- `aChangeIsPreviewedAndDiffed`: a part and a `batch` previewed, with
  the schematic, undo index and unsaved flag unchanged; `simulate` has no
  preview; `diff` against the saved file, steps back and another path.
- `theJsonFormIsCheckedAndRoundTrips`: parts, a flag block, labelled
  wires; a non-numeric value noted; `get_schematic` json taken back to the
  same text; an unknown property, an unknown type, two of a name, a slanted
  wire, text with JSON each refused with the schematic left; the `Vpulse`
  trap; a parameter's name not noted.
- `aScriptRunsTheTools`: loops, log and value; an atomic failure told with
  its line and put back; a runaway script stopped; no `run_script` from a
  script.
- `theProgramServesOverStdio`: `qucs-s --mcp-server` answers initialize
  and lists the tools. It builds a schematic from JSON, saves it, gives its
  netlist map and answers an unknown method with −32601. A save and a
  resource list sent together are taken in turn. It exits with 0 when its
  input ends.

`test_claude_code` (36 functions), new:
`theServerAsksAndWhatCannotBeUndoneIsAskedAbout`. A fake `claude` sends
four elicitation requests: yes/no, a choice of three, a form, and one
cancelled. The card's buttons and fields are used, and the answers
written back are checked. A tool use that cannot be undone is then asked
about after the tools were allowed, without "allow all", and one that can
be undone is not.

Deliberate breaks were each caught:

- The dock allowing an irreversible use once the tools are allowed:
  `test_claude_code`.
- The preview not putting the schematic back: `test_mcp_server`.
- `structuredContent` left out: six functions of `test_mcp_server`.

The full suite passes: 75 tests. `test_mcp_server`, `test_claude_code`
and `test_qucs_control` also pass under ASan and UBSan, the end-to-end
case running the ASan build of the program.
