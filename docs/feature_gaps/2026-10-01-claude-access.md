# Feature gaps: what Claude can and cannot reach in the window

*1 October 2026 — Qucs-S 26.1.5.*

Claude wrote down what of the Qucs-S window it can reach and what it cannot
(`qucs-s-claude-access.md`). It checked 26.1.5 against the running app
(`list_actions`) and the source, part by part:

- the menu bar;
- the toolbars;
- the docks and their buttons;
- the canvas's right-click menu;
- what only the mouse does.

Each finding was checked first, against the source and through
`qucs-s --mcp-server` with its own workspace, settings and HOME. The changes
are in `825e67d`. Each has a test, and each was broken on purpose to see its
test fail.

One finding reads differently under `--mcp-server`. There Qt runs on its
offscreen platform, which has no system dialogs, so a file dialog was Qt's
all along. `get_dialog` already read it as a form, and `set_dialog` filled it
in: a path in "File name", then Open. The native panel the audit met is that
of the window on macOS.

## Where it fell short

| Audit | Now |
|---|---|
| **About 14 menu entries open a native file or folder dialog** that `get_dialog` cannot see, left waiting for the user. Among them are Switch Workspace and Build Verilog-A module from subcircuit, which nothing else does. | While Claude clicks, a file dialog is Qt's own. That covers a menu action (`trigger_action`) and a dialog's button (`set_dialog`), Export as Image's Browse, say. Qt's `AA_DontUseNativeDialogs` is set around the click and put back after it: a modal dialog's click returns when it is answered, so it holds until then. `get_dialog` reads such a dialog as a form, and `set_dialog` answers it: a path in "File name" (or "Directory" for a folder), then Open, Save or Choose. The dialogs the user opens stay the system's. Every entry of the audit's list is reached so, Switch Workspace and Build Verilog-A module from subcircuit among them. The server's instructions and `trigger_action`'s description say how to answer one. |
| **Refused outright:** File > Open, Save As, Print, Print Fit to Page, Exit. Open and Save As as "a dialog of the system, which cannot be filled". | Still refused, and each now says why. Open and Save As: `open_document` and `save_document` do them (and say more than the dialog would). Print and Print Fit to Page print on paper, which Claude does not do: `export_image` writes a picture. Exit: Claude does not quit Qucs-S. |
| **The simulation console's Stop** is only there for a run Claude started (`simulate`'s `timeout`); **its Clear** is not reached at all. | Simulation > **Stop Simulation** and **Clear Simulation Console** are menu actions, the console's own (`SimulationConsole::stopAction()`, `clearAction()`). The user has them in the menu, and `trigger_action` reaches them. Stop is enabled only while a run goes, as its button is, and stops a run whoever started it. `simulate`'s description says so. |
| **The Content panel's and the File Browser's right-click menus** (Rename, Delete, Move to Trash) are not reached. Claude was left with `mv` and `rm`: an open document's tab then points at a file that is not there, and `rm` deletes for good. | Two tools. **`rename_file`** renames a file or folder, or moves it ('to' a name, or a path: into a folder there, or as the name it ends in), as the File Browser's rename does. The documents open from it follow, their tabs renamed and unsaved changes kept. It says when the suffix changed, and that a schematic's datasets keep their names. **`trash_file`** moves it to the system's trash. The documents open from it close; it is refused while one has unsaved changes, and while a simulation runs. Nothing is deleted when the trash cannot take it. It is asked about every time, as `clean_scratch` is. Both refuse the workspace, the home folder and the project open now. The instructions name them for this, rather than `mv` and `rm`. `QUCS_TRASH_DIR` makes "the trash" a folder of one's own, as `QUCS_SETTINGS_DIR` does for the settings: the tests use it, and no one's trash fills. |
| **Every toolbar button is a copy of a menu action**, so `trigger_action` reaches them all. | Confirmed, and now held by a test: a toolbar button that is no menu action fails `everyToolbarButtonIsInAMenu`. The one toolbar item that is no action, the simulator's list, is `set_simulator`'s. |

## Found on the way: the tests' trash

On macOS, two of `test_qucs_control`'s tests had put files into the user's
trash at every run:
- `import_data`'s `remove`;
- `clean_scratch`.

Only the Linux tests had kept the trash apart, through `XDG_DATA_HOME`. One
of this round's break tests did it too, before the fix: with `trash_file`'s
refusal during a run taken away, an empty test file (`access-note.txt`) went
to the user's trash.

What Qucs-S moves to the trash now goes through one function,
`misc::moveToTrash`, which `QUCS_TRASH_DIR` sends into a folder. It covers:
- the File Browser's Move to Trash, and its Replace;
- Delete Project;
- `trash_file`;
- `clean_scratch`;
- `import_data`'s `remove`.

`useIsolatedSettings` sets it for every test. The File Browser's and Delete
Project's trash tests, Linux-only before, run on macOS too. Each of these
tests now checks that the file went into the test's own trash. The README
gives the variable, beside `QUCS_SETTINGS_DIR`.

## Left as they are

| Audit | Why |
|---|---|
| The message dock's Copy (admsXml's output) | `build_verilog_a` returns the build's output with each error's line. |
| Typing into the Octave, Terminal and Python Shell docks | By choice. A Qucs-S tool that typed into a shell would run commands outside Claude Code's own rules for Bash, which ask as the user set them. Claude runs commands with Bash, and the tools with `run_script`. |
| The find bar's buttons | `get_schematic` and `get_netlist` give what Find searches. |
| The Tools programs (filter synthesis, line calculation, ...) and the S-parameter diagrams' Power matching and 2-port matching | They are programs of their own, outside the Qucs-S window. |
| The Claude Code panel's own controls | The user's, by design. |
| Placing, dragging, wiring, double-clicks, markers, tabs | Reached already, as the audit says, through the tools. |

## Tests

- **`test_qucs_control`:**
  - `fileDialogsClaudeOpensAreQts`:
    - File > Examples' dialog is Qt's while open, filled with a path and Open, the setting back after it;
    - Export as Image's Browse, a dialog's button: Qt's, and still so after it while the export dialog is open;
    - the user's own File > Examples is the system's;
    - Switch Workspace answered with a folder;
    - the refusals and what each says.
  - `everyToolbarButtonIsInAMenu`
  - `theConsolesStopAndClearAreReached`:
    - in `list_actions`, Stop disabled while idle and refused then;
    - Clear clears;
    - a slow run stopped through `trigger_action`;
    - `trash_file` refused while it runs.
  - `filesAreRenamedAndTrashed`:
    - renamed in place and moved by a path, an open schematic's tab following each way;
    - the suffix and Data Set notes;
    - the refusals;
    - trashed: a file, one whose name the trash has already, an open document (closed, or refused with unsaved changes), a folder holding one;
    - never the workspace or the project open now;
    - `trash_file` irreversible, `rename_file` not.
- **`test_simulation_console`:** `theMenuStopsTheRunAndClearsTheConsole`. The two actions are in the Simulation menu. Stop follows the run and stops it, and Clear clears the console and its status log.
- **`scripts/ci/check-tool-arguments.py`:** 73 tools, 0 disagreements.

**32 breaks, each caught**:
- the dialogs:
  - no Qt dialogs around a menu action's click, or around a button's;
  - the setting never put back, or put back to off rather than to what it
    was;
- the refusals: File > Open and Save As not refused; the printing message;
- `simulate`'s description;
- the console:
  - Stop or Clear not in the menu, or not connected;
  - Stop enabled while idle;
  - Stop's shortcut command not registered;
- `rename_file`:
  - a moved document's tab not told;
  - the workspace and the open project not refused;
  - into itself;
  - something there already;
  - the suffix asked after the rename;
- `trash_file`:
  - unsaved changes not refused;
  - documents not closed;
  - not refused during a run;
  - a second file of one name in the trash;
  - not irreversible;
  - `rename_file` not dispatched;
- each of the five trash calls deleting instead (on macOS a break that sent
  them to the system's trash would fill the user's).

Two were caught only after their test was sharpened:
- a button's Qt dialogs: a dialog the user opened, whose Browse Claude
  presses;
- `clean_scratch`: where its files went.

One found a fault: the suffix note was asked of the old path after the
rename, when nothing was there.

The full suite passes (80 of 80), and so do the scenarios s1 to s10
(77 checks). Two things in the user's folders were checked before and after
the final runs:
- `~/QucsWorkspace` and `~/Library/Caches/qucs-s` are as they were;
- the user's trash was not changed.

One more was found on the way, and left for another change: the Content
panel's own Delete still deletes for good, asking first. The File Browser's
Move to Trash and `trash_file` use the trash.
