# MCP efficiency: the check of 953c8c9, answered

The reviewer's file: `~/Desktop/qucs-reviews/2026-10-19-claude-mcp-efficiency-check.md`. It ran in the open app on build ac8ce07.

**The verdict:** five of six items passed. Item 1 was not checked: a resumed session does not show its tool set. There were three nits.

Each item was reproduced first over `qucs-s --mcp-server`, with an isolated home and settings.

## The items

| Feedback | Now |
|---|---|
| **1. The tools in every turn follow what is open: not checked.** A resumed session does not show its set. Start new sessions in three states. | **Checked, three new sessions over stdio**, the same server and tool list the dock's sessions get (table below). **`get_state` now says it**, so it can be seen from inside any session, resumed too: "tools in every turn": "the core, a model's (fem_*), the wiring - as a session starting now has them". A session's tools are listed when its program starts, new or resumed. |
| 2. A task's tools in one search: pass. | As it was. |
| 3. A kind of diagram's options folded: pass. | As it was, and nit 1 below. |
| 4. The format's properties a line each: pass. | As it was. |
| 5. A dialog that waits says what it reads: pass. | As it was, and nit 2 below. |
| 6. An action not named as it is: pass. | As it was. |

Item 1's three states, each in a new session:

| State | In every turn | `get_state` says |
|---|---|---|
| A project with a `.qfem`, none open | 25 tools: the model's, whole (as `describe_tool` gives them); no wiring | the core, a model's (fem_*) |
| A project without a model | 25 tools: the wiring; no model's | the core, the wiring |
| A model open, and a schematic with parts | 31 tools: both | the core, a model's (fem_*), the wiring |

## The nits

| Nit | Now |
|---|---|
| **1. `smith_circles`' folded text cut inside a parenthesis**: "... at 'frequency' (null; its fields ...". | **The first clause is cut at a ';', '.' or ':' and a space outside any bracket.** `smith_circles` now reads "A Smith chart's circles (type smith or admittance_smith) of the two-port its traces' run is of - its S-parameters (ngspice's ac.s_2_1, Qucsator's S[2,1]) at 'frequency' (null: the middle of the sweep); its fields circles, frequency (describe_tool add_diagram says each)". A test checks every folded option of `add_diagram` and `edit_diagram` for balanced brackets. |
| **2. The dialog comes back as escaped JSON**, in the `summary` of `trigger_action`'s answer. | **Found to be wider than the dialog.** Claude Code gives the model a successful result's `structuredContent` in place of its text. The server gave every text answer one, `{"summary": text}`. So every plain-text answer reached the model with its quotes and line breaks escaped: `save_document`'s, `get_netlist`'s, the `.sch` text of `get_schematic`, a dialog's JSON inside `trigger_action`'s. In the dock's 106 sessions, 673 answers were so, 8.6 % longer than their text. (The efficiency write-up had said the model gets the text alone; it was checked on a JSON answer only, and is corrected there.) **Now:** `structuredContent` is given where the answer is JSON (an object as it is, a list as its items), and for an error: Claude Code shows an error as its text anyway, and other clients keep its `{error}`. A plain text answer has none, and reaches the model as it is. **And as asked:** an answer that opened a dialog is JSON, with the dialog in a `dialog` field, its quotes not escaped: `trigger_action`'s and `context_menu`'s (`{"done": "...: it opened “...”, which waits for an answer (set_dialog answers it)", "dialog": {...}}`), and `send_input`'s ("opened" and "dialog"; a dialog closed and another opened: "closed" apart). The refusal, an error, stays text with the dialog in it, plain. |
| **3. The About box's tabs control labelled with the license line**, the label beside it; that text twice. | **A tab widget takes a label from a form layout or a buddy, else "tabs".** A label beside or above it is not its own. The About box's control is now "tabs", and the license line appears once, in `texts`. This was older than 953c8c9: `get_dialog` labels controls this way. |

## Tests

- **`test_mcp_server`:**
  - `resultsAreStructured`: a text answer has no `structuredContent` (a new document; a schematic's `.sch` text, its lines as they are); a JSON answer has it; an error has `{error}`; `isJsonAnswer`.
  - `inEveryTurnIsWhatIsOpen`: `get_state`'s "tools in every turn" in each of the states; every folded option's brackets balanced.
- **`test_qucs_control`:**
  - `aDialogIsReadFilledAndAnswered`: `trigger_action`'s `dialog` an object; the About box's tabs control "tabs", no label with the license line; check boxes not among the buttons.
  - `rightClickMenusAreOpenedAndChosen`: `context_menu`'s `dialog` an object with its title.
  - `rawInputIsSent`: `send_input`'s `dialog` an object.

Nine changes were broken on purpose, and all nine were caught:

| Break | Caught by |
|---|---|
| R1 text answers structured again | resultsAreStructured |
| R2 errors not structured | noSimulatorIsSaidSo (and the others that read `{error}`) |
| R3 trigger_action's dialog a text | aDialogIsReadFilledAndAnswered |
| R4 context_menu's dialog a text | rightClickMenusAreOpenedAndChosen |
| R5 send_input without its dialog | rawInputIsSent |
| R6 the fold cut inside brackets | inEveryTurnIsWhatIsOpen |
| R7 a tab widget's label beside it | aDialogIsReadFilledAndAnswered |
| R8 get_state without the tiers | inEveryTurnIsWhatIsOpen |
| R9 check boxes among a dialog's buttons | aDialogIsReadFilledAndAnswered |

Not tested: a `send_input` that closes one dialog and opens another at once, whose answer puts "closed" apart.

## Runs

- **Release:** the changed functions pass, `test_mcp_server` whole (17 of 17), and the Release suite 131 of 131.
- **The scenarios** (`scripts/mcp-e2e-scenarios.py`, the build's app): 97 checks, none failed.
- **Sanitizers** (ASan and UBSan): `test_mcp_server` and the three changed `test_qucs_control` functions pass, with no report.
