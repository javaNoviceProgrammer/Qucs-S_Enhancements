# The MCP tools, faster and leaner for the Claude in Qucs-S

The user (2026-10-19): *"Now I'd like you to focus on the MCP implementation. Make it as fast and as efficient for Claude that's running inside the qucs."*

## How it was measured

The Claude Code dock's own sessions are the evidence: the 118 transcripts in `~/.claude/projects/-Users-meisam-QucsWorkspace/`, 25 September to 19 October. The scripts read only aggregates: which tool, the size of its answer, its time, whether it failed, the turn's token counts.

- **Sessions:** 106 used the qucs tools, with 6,447 calls.
- **Text:** the answers' text came to 9.06 million characters.
- **Images:** 466 images came to about 635,000 tokens, at the API's (width × height)/750 after its scaling.
- **The first turn:** a session began with a median of 43,528 tokens of context (system, tools, prompt).
- **Tool searches:** there were 445, in 95 of the sessions. Each is a model round trip before the tool it loads can be called.

Where the time goes:
- **Qucs-S itself:** timed over stdio, an editing tool takes 0–6 ms in Qucs-S (`add_component`, `delete`, `undo`, `get_schematic`, `save_document`).
- **The dock:** the transcripts show 1–2 s for the same calls. That is the dock's permission check (in auto mode, a classifier) for every tool that is not read-only; the read-only tools, which are allowed outright, show 0.0 s.
- **So:** what can be made faster is how many turns a task takes and how many tokens each turn carries.

What was already right:
- **Compact JSON:** the answers are compact JSON, not indented.
- **No doubling:** the model receives the text alone, and `structuredContent` is kept beside it, not sent again. This was checked in the transcripts.
- **Change notes:** the notes of what changed since the last call are 0.1% of all answers.

## What changed

### 1. The tools in every turn follow what is open

The set loaded in every turn (`anthropic/alwaysLoad`) was fixed, chosen for drawing schematics. It did not match how the tools are used. Counting the tools inside batches too:

| Always loaded, used in few sessions | Searched for, used in many |
|---|---|
| `add_wire` 16, `move` 5 | `fem_describe` 40, `fem_solve` 37, `fem_evaluate` 36 |
| `undo` 7, `connect` 25 | `close_document` 38, `list_actions` 34, `fem_model` 33 |
| | `new_document` 31, `fem_edit` 30 |

Sessions of a model searched a median of 3 times (a mean of 4.5). Only 7 of the 44 sessions of a model also wired a schematic.

**Now** `QucsControl::tools()` is worked out when the client asks for the tools, at the session's start:
- **Always:** the core set, now with `close_document`, `new_document` and `list_actions`.
- **A model open, or a `.qfem` in the open project's folder:** the model's tools, `fem_describe`, `fem_model`, `fem_edit`, `fem_solve`, `fem_evaluate` and `fem_plot`. They come whole, as a search would load them, with no summary to look past.
- **A schematic with parts open, or no model:** the wiring tools, `connect`, `add_wire`, `set_label`, `move`, `delete` and `undo`.

| Context | Tools in every turn | Characters |
|---|---|---|
| Before (any) | 22 | 38,745 |
| No model (a fresh window) | 25 | 40,490 |
| A model open | 25 | 49,844 |
| A model and a schematic with parts | 31 | 57,801 |

A model session pays about 2,400 tokens more per turn (cached) instead of three or more searches.

### 2. A task's tools found in one search

174 of the 445 searches named one tool and 91 named two. The instructions now say to load a task's tools together, in one search, and name the families:
- a model's tools;
- a plot's: diagrams, traces and markers;
- the window's: actions, dialogs, panels;
- the files'.

### 3. A kind of diagram's own options folded

`add_diagram` and `edit_diagram` published 22,248 and 20,145 characters, loaded in 43 and 32 sessions and in every turn after. Three quarters of that was the nested options of 14 kinds of diagram: eye, bathtub, contour, Bode and so on. Only 13 of 367 diagram calls set any of them.

**Now:**
- **Published:** each kind's options are published as their description's first clause and their fields' names: "An eye diagram's own (type eye); its fields drawn, from, levels, mask (height, width), measurements, span, threshold, unit_interval (describe_tool add_diagram says each)".
- **Unchanged:** `describe_tool` still gives the whole schema, and a call is still checked against the whole schema. `{"eye": {"span": "wide"}}` is refused as before, at `eye.span`.
- **Sizes:** `add_diagram` is now 11,982 characters and `edit_diagram` 9,893.

### 4. The format's properties a line each

`fem_describe` `what: "format"` was the largest answer of all: 95 calls and 1.06 million characters, 11.7% of all the answers' text. Sessions read one to seven groups of it.

Each kind's properties were objects with named fields, so a group's kinds were two thirds JSON names and marks. **Now** each property is a line:
- `Eg: Band gap Eg [eV] = 1.12[eV]; when source=user`
- `source: From = material; one of material, user`
- `selection: Selection (selection) = {"all":true}`

The answer's `properties as` field explains the line once.

| Format answer | Before (characters) | Now |
|---|---|---|
| The kinds, all groups | 188,440 | 125,990 |
| Group `semi` | 59,856 | 50,576 |
| Group `es` | 19,308 | 18,637 |

The rest of a group's answer is the format text's own paragraphs, which say how a file is written.

### 5. A dialog that waits says what it reads

**The problem:** 26 calls were refused because a dialog was open ("About Qucs" left open ten times). The recovery was usually four turns:
1. a search for `get_dialog`;
2. `get_dialog`;
3. a search for `set_dialog`;
4. `set_dialog`.

Opening a dialog with `trigger_action` likewise answered only its title.

**Now** four answers carry what the dialog reads, as `get_dialog` gives it: the refusal, `trigger_action`'s, `context_menu`'s and `send_input`'s "opened".
- **Short dialogs** (2,000 characters or less) come whole.
- **Longer ones** give their push buttons, without the check boxes. Those are controls for `set_dialog`'s `set`, not answers.

So `set_dialog` answers the dialog at once.

### 6. An action not named as it is: the nearest said

`trigger_action` failed when an action was named as Claude guessed it ("Help > About Qucs...", "View > Edit Circuit Symbol"), and Claude then listed every action.

**Now** the refusal names the actions whose last part has the most of the words asked, at least half of them and up to five: "There is no action Help > About Qucs.... Meant Help > About Qucs-S?". When nothing is near, it says what it said before.

## Not changed, and why

- **The permission check:** it costs 1–2 s on every editing call in the dock, against 0–6 ms of Qucs-S's own time. Allowing the undoable editing tools without asking would remove it, but which tools are asked about is the user's choice of permission mode. That is for the user to decide; it was not done here.
- **Screenshots:** they are about a fifth of what the answers cost, around 1,400 tokens each. Most are already at the size the API scales a picture to, and a smaller default would lose what a schematic's picture is for.
- **A model's tree** (`fem_model` open, `fem_describe` model): properties equal to their kind's default are 2–8% of it on the examples.
- **A dialog titled "%1"** in two transcripts was not a dialog: it was Claude reading Qucs-S's own source.

## Tests

- **`test_mcp_server` `inEveryTurnIsWhatIsOpen` (new):**
  - with no model, the wiring is loaded and not the model's tools;
  - a model made with `fem_model` brings its tools, each described whole as `describe_tool` gives it, and the wiring goes;
  - a schematic with a part brings the wiring back;
  - a project folder with a `.qfem`, and none open, brings the model's tools;
  - `add_diagram`'s `eye` is folded with its fields named, and `describe_tool` still has the whole;
  - `{"eye": {"span": "wide"}}` is refused at `eye.span`;
  - the instructions say "in one search".
- **`test_mcp_server` `theToolsAreAnnotatedAndTiered`:** a model's tools in every turn are described whole.
- **`test_multiphysics_doc` `claudesFormatOfAGroup`:** the semi group's lines, as above, and `properties as`.
- **`test_qucs_control` `aDialogIsReadFilledAndAnswered`:**
  - the refusal and `trigger_action`'s answer carry the dialog's controls or its buttons, with no check box among the buttons;
  - "Help > About Qucs..." is answered "Meant Help > About Qucs-S?";
  - a name with nothing near is answered as before.

Sixteen changes were broken on purpose, and all were caught:

| Break | Caught by |
|---|---|
| B1 a model open not seen | inEveryTurnIsWhatIsOpen |
| B2 the wiring in every context | inEveryTurnIsWhatIsOpen |
| B3 a project's models not seen | inEveryTurnIsWhatIsOpen |
| B4 a model's tools not whole | inEveryTurnIsWhatIsOpen |
| B5 the core without the documents' tools | inEveryTurnIsWhatIsOpen |
| B6 no folding | inEveryTurnIsWhatIsOpen |
| B7 folded without the fields' names | inEveryTurnIsWhatIsOpen |
| B8 a call's types not checked against the whole schema | inEveryTurnIsWhatIsOpen |
| B9 the instructions' one search | inEveryTurnIsWhatIsOpen |
| B10 properties as objects | claudesFormatOfAGroup |
| B11 no 'properties as' | claudesFormatOfAGroup |
| B12 the refusal without the dialog | aDialogIsReadFilledAndAnswered |
| B13 an opened dialog not said | aDialogIsReadFilledAndAnswered |
| B14 check boxes among the buttons | aDialogIsReadFilledAndAnswered |
| B15 no near action | aDialogIsReadFilledAndAnswered |
| B16 every action near | aDialogIsReadFilledAndAnswered |

## Runs

- **Release:** the changed functions pass offscreen, and `test_mcp_server` passes whole, 17 of 17.
- **Sanitizers** (ASan and UBSan): the changed functions pass in 4–9 s each, with no report.
- **The Release suite:** 130 of 130.
