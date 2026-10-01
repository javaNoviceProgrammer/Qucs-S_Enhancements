# Feature gaps: what Claude still needed for full control

*1 October 2026 - Qucs-S 26.1.5.*

Claude set out what it still could not reach in the Qucs-S window
(`qucs-s-claude-access.md`, second version). It came in seven groups, each with a
proposal:
1. any widget;
2. text documents;
3. the consoles;
4. the synthesis programs;
5. typed settings;
6. waiting and a last resort;
7. what to keep out of its reach.

The user decided two of them:
- **The consoles (3):** typed into, asked about every time.
- **The synthesis programs (4):** parked as a proposal.

Everything else was built. Each finding was checked against the source and through
`qucs-s --mcp-server` with its own workspace, settings, trash and HOME. Each change has
a test, and each was broken on purpose to see its test fail.

The work is in five commits:
- `403d5a0`: group 1;
- `0b4fd65`: group 2;
- `019aa65`: groups 3 and 5;
- the commit with this write-up: group 6, the open project in `get_state`, and what
  was found on the way;
- the proposal for group 4 (`docs/proposals/2026-10-01-synthesis-tools.md`).

## 1. Any widget

| Audit | Now |
|---|---|
| `trigger_action` walks the menu bar and `get_dialog` sees dialogs. **The docks, panels, toolbars, status bar, tabs and right-click menus are out of reach.** | **`get_ui`** and **`set_ui`** read and use any part of the window as `get_dialog` and `set_dialog` do a dialog (`describeControls` and `fillControls` are shared). That covers a dock or a panel of one (`dock:Content`, `dock:Problems`, `dock:Simulation`), a toolbar, the status bar and the documents' tabs. Views come row by row, and rows can be selected, opened, expanded and collapsed. Sliders move as a drag would; logs give their end. While an area is shown, each control has its rectangle in the area's pixels (`at`). **`context_menu`** opens the right-click menu of a part, a diagram, a point of the canvas, a Content or Projects row, a tab or a file. It lists the menu, and chooses from it as the keyboard would. |

## 2. Text documents

| Audit | Now |
|---|---|
| No tool reads or edits a text tab (`.cir`, `.va`, `.m`, `.py`): Claude edited the file on disk, which a tab with unsaved edits does not take. | **`get_text`** reads the tab as the window has it, unsaved edits included: numbered lines, its revision, the cursor, the selection and marks. **`edit_text`** makes find and replace edits or edits by lines, worked out on a copy, all or none, as one undo step; a stale `revision` refuses them. **`goto_line`** shows a line. `build_verilog_a` marks its errors in the open `.va` tab: a wavy line, a dot in the margin and the message as a tooltip. |

## 3. The consoles

| Audit | Now |
|---|---|
| The Octave, Python Shell and Terminal docks can't be typed into. | **`console`** types one line into the dock, shown there as typed. It returns what was printed until the prompt came back, or says "still running"; with `interrupt`, Ctrl-C stops the run. Without `input` it reads the last lines. **Asked every time**, as the user chose. Since this round that holds also where Claude acts on its own (`auto`): see "Found on the way". `get_ui` reads the consoles, while `set_ui` and `send_input` refuse them. |

## 4. The synthesis programs

| Audit | Now |
|---|---|
| Filter synthesis, Active filter, Line calculation, Attenuator, Power combining, the S-parameter Viewer and the Receiver calculator are programs of their own. Started by `trigger_action`, they are out of reach. | **Parked as a proposal**, `docs/proposals/2026-10-01-synthesis-tools.md`. It has the facts of each program: its size, the entry point of its calculation, and what ties that to its window. The filter's `QucsSettings` collides with Qucs-S's, and the line calculation reads its values through its window. It weighs three ways to reach them and recommends the calculations in Qucs-S as tools, in order of cost: attenuator, active filter, LC and line filters, line calculation, power combining. Matching Circuit is a dialog of Qucs-S, already in reach through `trigger_action` and `set_dialog`. |

## 5. Typed settings

| Audit | Now |
|---|---|
| The settings dialogs are reached only through `set_dialog`, a control at a time by its label. | **`get_settings`** reads Application, Simulators, CDL and a document's settings by typed keys ("Tab/Label"): text, bool, choice and its choices, number and its range, table. Nothing is shown. **`set_settings`** sets them through the dialog's own OK, so what the window does after it is done. Each change comes back with what it **was** (set back with that) and what it is **now**, read again. A message the dialog shows on the way is answered and told. Claude Code's own settings are refused. A field labelled from above it (the Simulators Settings' paths) has a key too. |

## 6. Waiting and a last resort

| Audit | Now |
|---|---|
| **`wait_for`** `{event: simulation_finished \| dialog \| document_changed \| file_written}` - to react without polling. | **`wait_for`** returns when it has happened, or at its `timeout` (60 s unless given, at most 3600) with `happened: false`, which is not an error. It is read-only, other calls go on meanwhile, and it isn't taken in a `batch` or a script, which every other call would wait for. The events: <ul><li>`simulation_finished`: a run's end. With `id`, that run's outcome as `simulate` gives it. Without, the run going now or the next one, the user's too, with whether it succeeded and its last lines.</li><li>`dialog`: one open already counts.</li><li>`document_changed`: by whom (the user, another conversation, its file), since `revision` from an earlier answer (no change is missed in between) or since now; or closed.</li><li>`file_written`: written, made or removed, then left alone for 400 ms, so it isn't read half-written.</li></ul> **An edit the user makes while Claude waits is the user's.** A pending call had held its caller as the editor of every edit until it answered, so the user's edit came back to Claude as "you". |
| **Background simulations**: `simulate` with `background: true` returns an id, then `simulation_status` and `stop_simulation`. The 3600 s limit with a blocking wait was awkward for Monte Carlo and long transients. | `simulate`'s **`background`** answers at once with the run's id; meanwhile other calls go on. Its `timeout` is then a limit at which the run is stopped, none unless given. **`simulation_status`** with an id says how long the run has gone and gives its last lines, then its outcome once it has ended, as `simulate` would have given it. Without an id it lists the runs followed. **`stop_simulation`** stops one, or without an id the run going, the user's too, and returns its outcome. A run's end is told in every conversation's next answer, and `get_state` lists the runs followed. **A foreground run past its `timeout` goes on the same way.** The answer gives its id, where the description had said it was stopped while the code let it run unfollowed. |
| **`send_input`** `{target, click, double, drag_to, keys}`: raw mouse and keyboard, the last resort, always followed by a screenshot. | **`send_input`** gives, in order, a click (`button` left, right or middle; `double`; `drag_to` in steps; `modifiers`), then `keys` (at most four, as `list_actions` writes shortcuts), then `text`. Targets: <ul><li>the canvas, in the schematic's coordinates, brought into view (or the picture's pixels with `pixels`);</li><li>a part of the window as `get_ui` names it, in its pixels, laid out as it is shown.</li></ul> A key that is an action's shortcut sets off that action, as the keyboard does, and the answer names it. Any other key goes to what has the focus there, and the clicked widget takes it as a click would. Qt sends a key that isn't the system's through the window's shortcuts all the same. So every action that has it is checked first: the window's, and any widget's own, including one of no owner shown on a widget of the panel. It returns what it opened, the status bar's message, and a picture of the target with how its pixels map. A dialog that opens is said, for `get_dialog`; a menu is read, closed, and pointed to `context_menu`. **Refused, with nothing sent:** the consoles, the Claude Code panel, a key that would quit Qucs-S or set off what `trigger_action` refuses, and the panel's own keys. **Asked about each time.** |
| **`read_help`** `{topic}`: the built-in help pages. | No help pages ship in this build: Help > Help Index and Getting Started open the online manual and a PDF in the browser. **`read_help`** searches what the build has, all words of `topic`: <ul><li>each menu action's own help (What's This, status tip, shortcut);</li><li>the component types (`describe_component_type` explains one);</li><li>the example schematics (`open_document` opens one);</li><li>any papers in the docs folder (`read_pdf`).</li></ul> It names the online manual's and the tutorial's addresses. Without `topic`, it says what there is. |
| **The open project in `get_state`** (`qucs-s-get-state-open-project.md`): only a screenshot of the Content panel told which project was open. | `get_state` has **`project`**: its name and folder, null when none is open. The note's optional second change is made as well: without `folder`, **`list_documents` lists the open project's files** (`project` says so), and still names the workspace's projects. The workspace's path lists the workspace. |
| **Undo for file tools**: `rename_file` can be undone with `undo files`; `trash_file` too would make every file change reversible. | Neither could be undone: `undo files` put back only the bytes a call wrote. A file step now also holds what was **moved**, from where to where. **`undo files`** moves it back: <ul><li>a rename to its old name, the open documents following;</li><li>a file or folder out of the trash.</li></ul> It won't move back over one there again, and says why. A thing no longer where it went is left. On macOS, if the system keeps an app out of `~/.Trash`, it says so and names Finder's Put Back. That case couldn't be tried here: the tests' trash is their own (`QUCS_TRASH_DIR`). The documents `trash_file` closed are not opened again. `undo_history` lists the moves. |

## 7. Out of Claude's reach

| Audit | Now |
|---|---|
| **The Claude Code panel**: its permission prompts, its settings, its conversations. | Out of reach everywhere: <ul><li>`get_ui` and `set_ui` refuse it, and don't list it;</li><li>`send_input` refuses it as a target, and its keys;</li><li>`trigger_action` refuses its actions, View > Claude Code among them (that one is the dock's own);</li><li>`set_settings` refuses Claude Code's settings.</li></ul> |
| **File > Exit**, and anything that throws away unsaved work unasked. | `trigger_action` refuses Exit, and `send_input` refuses its key. `close_document` asks unless `unsaved` is given, and with `discard` it is asked about every time. `trash_file` refuses a document with unsaved changes. |
| **The git bar's Push Branch and Create Pull Request.** | The git bar is inside the Claude Code panel, so it is out of reach with it. |

## Found on the way

- **The Delete key on the macOS canvas never deleted anything.** The shortcut manager
  registered Edit > Delete as `QKeySequence(QKeySequence::Backspace,
  QKeySequence::Delete)`: two StandardKey enum values, 69 and 7, read as keys. That
  made the chord "E, Ctrl+G" (upstream code). The key labelled "delete" (Backspace)
  is now the command's key, and forward Delete is taken beside it while the command
  keeps its default (`QucsCommand::setAlsoByDefault`). A shortcut the user changed
  is that key alone. `send_input`'s test found it: "Delete" set off nothing.
- **In `auto`, a console line was not asked about.** The panel let through every use
  "that cannot be undone" where Claude acts on its own, the console's line too. Its
  question also said "files are deleted or written over". The host now names the
  uses asked about every time, with why (`ToolHost::askedEachTime`): a line typed or
  Ctrl-C sent into a console. They are asked in `auto` too, never with "allow all";
  only `bypassPermissions` skips them.
- **The user's edits during a call that waits were Claude's.** A call kept its
  caller as the editor until it answered. So during a `console` line running, a
  `wait_for`, or a foreground `simulate`, the user's edit came back to Claude as
  "you". Each now releases its caller once it waits. A run's own late edit, the
  optimum written in with `apply_optimum`, is its caller's still.

## Tests

- **`test_qucs_control`:**
  - `backgroundSimulationsAreFollowed`:
    - answered at once, while it runs, and in `get_state`;
    - the status while it runs;
    - its end waited for, with the outcome;
    - the note in a conversation's next answer;
    - stopped by its id;
    - a foreground run past its timeout followed by its id;
    - a background run stopped at its timeout;
    - a run the user starts, waited for without an id;
    - the user's edit while a conversation's run goes, told as the user's;
    - the refusals.
  - `waitForWaitsForEvents`:
    - a dialog, at its timeout and then when it comes;
    - the user's edit while Claude waits, told as the user's;
    - since a revision;
    - a file written twice, told once, settled;
    - an event it doesn't know, no file named, in a batch.
  - `trashAndRenameAreUndone`:
    - renamed and back, an open schematic's tab following;
    - a file and a folder taken back from the trash;
    - not over one there again;
    - `undo_history`.
  - `theOpenProjectIsInTheState`: null, then the project, then null; `list_documents`
    lists the project's files and the workspace's projects, and the workspace by its
    path.
  - `rawInputIsSent`:
    - Edit > Delete's keys, a changed one, set back;
    - a click selects; a drag moves; Delete deletes;
    - a double click opens the part's dialog, while other tools are refused;
    - a right click's menu read and closed;
    - the refusals, with nothing sent: quit, a key of an action of no owner shown on
      the panel, View > Claude Code, a console, the panel itself, tabs, nothing
      given;
    - a panel's field found by `get_ui`'s `at`, clicked and typed into;
    - a point outside.
  - `helpIsReadFromTheBuild`: what there is, an action's own help, an example, a
    component type, nothing found.
  - Also:
    - `consolesAreTypedIntoAskedEachTime` checks `askedEachTime`, and that a command
      quiet for 3 s, its prompt not back, is not over;
    - `theConsolesStopAndClearAreReached` checks `simulate`'s description;
    - `anOptimumIsReportedAndApplied`: the optimum `apply_optimum` writes in is
      the edit of the conversation that ran it.
- **`test_claude_code`:** `aConsoleLineIsAskedAboutEvenInAutoMode`. In `auto`:
  - an irreversible use goes through unasked;
  - a console line is asked, with its reason and no "allow all";
  - in `bypassPermissions` it is not asked.

The arguments checker (`scripts/ci/check-tool-arguments.py`) checks 87 tools.

**Through `qucs-s --mcp-server`, isolated** (its own HOME, settings, workspace and
trash):
- the server lists 87 tools; `wait_for` and `read_help` are hinted read-only, and
  `send_input` destructive;
- `read_help` finds Simulation > Tune and its F3 for "tuner";
- a click on the canvas and then "Delete" delete the part, the answer naming the
  action;
- the Terminal is refused as a target.

The e2e scenarios (`scripts/mcp-e2e-scenarios.py`): 77 of 77. The full suite: 80 of 80.

## Breaks

Each change was broken on purpose, one at a time, and its test run.

- **Groups 1 and 2:** 20 and 16 breaks, each caught (`403d5a0`, `0b4fd65`).
- **Groups 3 and 5:** 17 breaks, each caught (`019aa65`). One wasn't caught at
  first: a console counted as done after 1.5 s of quiet with its prompt not back.
  Since then a command quiet for 3 s must still come back with what it printed
  after.
- **Group 6, and what was found on the way:** 45 breaks, each caught in the end.
  - `simulate` with `background`: no answer at once; its end not kept; a run past its
    timeout not followed; a background run not stopped at its timeout;
    `stop_simulation` not stopping; a user's run waited for before it began; the runs
    not in `get_state`; their end not told; the caller held during a run.
  - `wait_for`: the caller held while it waits; `revision` not taken; a file read
    before it settled; a dialog not seen; taken in a batch; its timeout an error.
  - Files: a trash or a rename not kept to undo; documents not following back; moved
    back over one there again; moves alone not a step; `undo_history` without them.
  - The project: no `project` without one; `list_documents` from the workspace; the
    projects not named.
  - `send_input`:
    - keys not matched to their action;
    - the panel's keys let through, by any of three ways: the refusal, an action
      shown on a widget of the panel, or the widgets' own actions not looked at;
    - View > Claude Code allowed;
    - a console as target;
    - a drag's first step lost;
    - a click not focusing;
    - `get_ui` without `at`;
    - no picture;
    - the canvas's points taken as pixels.
  - Edit > Delete's chord back; its extra key kept after the user changed it.
  - `apply_optimum`'s edit not its caller's.
  - `read_help`: no actions, no examples, any word enough.
  - The panel: a console line skipped in `auto`; its reason not shown; "allow all"
    offered; asked in `bypassPermissions`.

  Three were not caught at first. A key matched to no action set it off all the same,
  because Qt sends a key through the shortcuts itself: the answer now names the
  action. The panel's key refusal was doubled by `refusedAction`'s: the test's action
  is now one of no owner shown on the panel, which only the new check sees. The
  canvas's points equalled its pixels at the test's zoom: the test zooms in first.
