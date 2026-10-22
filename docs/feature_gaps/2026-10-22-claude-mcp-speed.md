# The MCP tools measured, and made leaner

The user (2026-10-22): *"focus on heavily optimizing and enhancing the MCP implementation so that it
becomes very fast and efficient"*. I measured the tools end to end before changing them, and changed
what the measurements pointed to. The server itself was already fast. The time went to three places:

- the dock's own GUI thread, between renders;
- Claude Code's permission check in auto mode;
- the size of what the tools send.

## Measured

**The server** (`qucs-s --mcp-server`, Release, a scratch HOME):

| | |
|---|---|
| start to the `initialize` answer | 0.4 s warm, 0.86 s cold (a fresh settings folder) |
| `ping` | 0.02 ms |
| `tools/list` (130 tools) | 3 to 7.5 ms, 254 KB |
| `get_state`, `list_documents`, `get_schematic`, `check_schematic` | 0.06 to 0.11 ms each (median of 30) |
| `get_netlist` | 0.74 ms |
| `open_document` | 7 to 80 ms (schematics, models, data displays) |
| `close_document` | 1 to 110 ms (a model just solved too) |
| 300 `get_state` calls written at once | 22 ms in all |

**The dock's sessions** (17 conversations since 19 October, 671 calls of Qucs-S's tools; Claude Code
2.1.304 writes how each call was allowed):

| how the call was allowed | calls | call to result, median | p75 |
|---|---|---|---|
| by a rule (`--allowedTools`: the tools that only look) | 93 | 0.12 s | 0.20 s |
| by Claude Code's classifier (auto mode: every other tool) | 30 | 0.68 s | 1.20 s |
| on 22 October alone, editing calls: by the classifier | 24 | 0.64 s | 1.20 s |
| on 22 October alone, editing calls: by a rule | 3 | 0.03 s | 0.03 s |

All 17 conversations ran in auto mode.

## M1: the dock answered between renders

The dock's MCP is answered on the GUI thread, between the panel's renders. Each render rebuilt and laid
out the whole transcript: 0.26 s at 40 replies, 0.72 s at 120, once every 40 ms while a reply streamed.
A call arriving meanwhile waited for the render to finish. Calls allowed by a rule (0.12 s, against less
than 1 ms in the server) and the longest conversations' calls (1.27 s, against 0.70 s at 150 to 400
thousand characters) show it.

The panel now draws only what changed, 1 ms at 120 replies (`87e3d9e`,
[its report](../bug_hunts/2026-10-22-claude-panel-fuzz.md), P1).

## M2: Qucs-S's undoable tools allowed in auto mode (asked of the user, and answered yes)

In auto mode, Claude Code's classifier decides every call that no rule allows, before the call reaches
Qucs-S: about 0.6 s per editing call (0.64 s median on 22 October, against 0.03 s for a call allowed by
a rule). The user was asked whether to spare it for Qucs-S's own undoable tools, and said *"yes,
pre-allow them in auto mode"*.

**Which tools.** `QucsControl::autonomousTools()` names them, through `ToolHost::autonomousTools()`. It
is an explicit list, so a tool added later is not pre-allowed until someone names it. A tool is on it
when each use of it does one of these:
- changes open documents by steps of Edit › Undo (`fem_edit`, `add_component`, `batch`, ...);
- adds a document, a project, or a dataset or export written anew (`new_document`, `simulate`,
  `fem_solve`, `import_data`, ...);
- shows something (`open_document`, `show_pdf`, ...).

Left out, and so checked as before:
- what moves, renames or deletes files (`trash_file`, `rename_file`, `clean_scratch`);
- what writes libraries;
- what drives the window as the user would (`trigger_action`, `set_dialog`, `set_ui`, `context_menu`,
  `send_input`, `console`);
- the settings (`set_settings`, `set_simulator`);
- git, `run_script`, `build_verilog_a`, `xml`.

**Started with them.** When the dock starts Claude Code in auto mode, those tools are in its
`--allowedTools`, beside the tools that only look.

**What the dock asks about itself.** A call that Claude Code lets through by a rule is checked by the
dock before it is made. That is any call of these tools or of the reading tools.
- **A use that cannot be undone, or that is asked about each time,** waits on the dock's permission
  card. Examples: a file written over, unsaved changes discarded, a batch holding such a call, a console
  line. Allowed, it is made; refused, Claude is told "The user did not allow this."
- **This covers two cases that went unasked before.** `get_netlist` writing over a file with `save_as`
  is one of the reading tools, which were always allowed. A `batch` was counted undoable whatever it
  held.
- **The Session's functions.** `asksItself()` makes this check, `hostRequest()` builds the question,
  and `a_held` keeps the call until it is answered.
- **A turn that ends** with a call waiting answers it "Not made: the turn was stopped."

**Rules that follow the mode.** Claude Code is started again, with `--resume`, when the mode moves into
or out of auto. That is at once between turns, and after the turn during one.
- Meanwhile a pre-allowed call is asked about as the new mode says.
- "Allow all Qucs-S tools" on such a card makes the other waiting calls too.
- Bypass mode asks nothing, the dock included.

**Rule changes that came with it.** `irreversible()` now counts:
- a batch, as its calls;
- `fem_model`, `create_subcircuit` and `import_netlist` with `replace`, which writes over a file.

`askedEachTime()` counts a batch as its calls too. The mode menu's tooltip for Auto says what is checked
now.

**Tests.**
- `autoModeAllowsTheUndoableTools`: the arguments; an undoable call made at once; calls that cannot be
  undone or are asked each time held, allowed, then refused; a reading tool's overwrite; a switch to
  asking during a turn, with two held calls and allow-all; the restarts both ways; a turn ending on a
  held call; bypass.
- `theToolsAllowedInAutoMode`: the list (every name a changing tool of the server's, none of the left-out
  kinds, no git); a batch's calls; `replace`.

## M3: `get_state` lists the workspace's projects by name

**Before.** `get_state` listed every project of the workspace as `{name, folder}`: the folder a full
path, the open one marked. The open one is also `project`. In the user's workspace that was 92 projects
and 7,349 of a `get_state`'s 10,703 characters. Every session asks for it first ("Start here").

**Now.** The projects are names, with a note only where it adds something:
- `"Project2 (folder Project2_prj)"`;
- `"shared (linked)"`.

Each project lives in the workspace's folder of its name. With the user's projects that is 1,635
characters, and `get_state` is 4,989, 53 % less. `list_documents` lists the workspace's projects in the
same form; it gave `{project, folder, linked}` objects. `get_state`'s description says so.

The tests are `theStateListsTheProjects` (with 40 more projects: no paths, less than 32 characters each),
`theWorkspaceIsListed` and `theOpenProjectIsInTheState`.

## M4: a PDF's tools in every turn while a PDF is open

The tools Claude Code loads into every turn follow what is open: the core, a model's, the wiring. With a
PDF open, `read_pdf` and `show_pdf` are now among them (3.4 KB). Each of the four recent sessions with a
paper open began by searching for them, a turn of its own each time. `get_state`'s
`tools in every turn` names "a PDF's".

The test is `inEveryTurnIsWhatIsOpen`: a PDF opened, then closed.

## M5: an answer's JSON read once

`callResult` read a JSON answer's text twice: once to see whether it is JSON, once to make its
`structuredContent`. Now it reads it once. On a 14 KB answer the difference is within the noise; on long
answers it saves a parse per call.

## Considered, not done

- **A learned set of tools in every turn.** The tools that most of the last sessions used were replayed
  over 118 sessions. At the same bytes per turn it would have spared 38 of 233 tool searches (16 %); at
  42 % more bytes, 58. One search usually loads several tools, some of them rare.
- **Telling Claude Code when the set changes** (`tools/list_changed`, a model opened). The tools head
  every request, so a change makes the next request read the whole conversation again uncached. A
  search adds a turn but keeps the cache.
- **Startup.** 0.4 s warm, once a session.
- **The window's own slowness.**
  - On 20 October, opening eight data displays took 9 to 16 s each, and closing or showing a model 1 to
    2.7 s (`probe.qfem` 7 s).
  - Headless, the same files open in 7 to 80 ms, and a model just solved closes in 110 ms.
  - It belongs to the window: the drawing of what comes in front, or code changed since. It was not
    reproduced without the user's window, and is left open.

## Breaks

5 breaks of M3 and M4, all caught:
- the projects with their full folders again;
- a folder named otherwise not said;
- `list_documents`' objects;
- the PDF's tools not in every turn;
- `get_state` not naming them.

M5 changes no answer and has no break.

18 breaks of M2, all caught (bypass's once a reading tool's overwrite was tried there):
- the tools not given at the start, or given in every mode;
- no restart leaving auto, or only after the next turn;
- each of the dock's own checks taken out in turn: the irreversible use, the use asked each time, the
  reading tools, the changed mode, bypass;
- a held call allowed but not made, refused but not answered, left at the turn's end, or answered as a
  `can_use_tool`;
- a batch's calls not looked at, either way;
- `replace` not counted;
- `trigger_action` or a git tool on the list.

The suites that read these answers pass: `test_qucs_control`, `test_mcp_server`, `test_wishlist_tools`,
`test_multiphysics_doc`, `test_claude_code`, the PDF, image and layout viewers. The changed ones pass on
ASan too.
