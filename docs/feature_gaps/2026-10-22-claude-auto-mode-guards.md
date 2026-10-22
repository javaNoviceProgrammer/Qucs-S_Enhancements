# Two guards for the Claude panel: a schematic's commands, files outside the workspace

The user (2026-10-22): *"Add two guards to the Claude dock in auto mode: ask before simulating a
schematic that runs shell commands, and ask before reading or writing files outside the workspace."*
These are findings A1, A2 and A5 of [the auto-mode hunt](../bug_hunts/2026-10-22-auto-mode.md). Then,
while the work was under way: *"Actually, let's add this as settings in the claude chat gui (the three
dot ... menu) so that the user can choose and decide."*

## The two settings

Under ⋯ > Permissions, below the modes, there are two items with tooltips. Both are on until the user
turns them off:
- **Ask Before a Schematic's Commands Run**;
- **Ask Before Files Outside the Workspace**.

They are kept in the settings (`ClaudeCode/askBeforeCommands`, `ClaudeCode/askOutsideWorkspace`). A
change applies to every conversation at once, because the host holds the guards, not each tab. Another
tab's menu shows the change when it opens, and a new tab starts with it.

A use either guard names is asked about each time, with the commands or the files on the card. There is
no "allow all" on that card. This holds in every mode but Bypass Permissions, as for a line typed into a
console (`askedEachTime()`), not in auto mode alone:
- in Ask Before Acting, the card for the call now says what it runs or which file it touches;
- in Accept Edits, and after "allow all Qucs-S tools", the host's tools went unasked;
- the tools that only look (`read_pdf`, `list_documents`, `get_dataset`, the git tools) went unasked in
  every mode.

Each item's tooltip says "in every mode but Bypass Permissions". Off, the guard names nothing, and the
dock behaves as it did before.

## A1. A schematic's commands

`simulate` and `tune` are asked about when the schematic they would run carries commands besides the
simulator, as `erc::commandsRun()` finds them. So is `trigger_action` of Simulation > Simulate,
Calculate DC Bias or Tune. What counts:
- a System Command part;
- ngspice's `shell` (or `system`, `!`) in a part's text, or in a library, include or subcircuit it
  brings in;
- the Octave script run after the simulation;
- new here: an Include Script part's SPICE text, which goes into the netlist as it is, through a
  `.control` block of its own or one in a file it includes;
- new here: a line that runs a file's commands or native code (`source`, `codemodel`, `osdi` and their
  `pre_` forms), in a part's ngspice text or in a `.control` block;
- new here: a `.spiceinit` left in the folder ngspice runs in, which ngspice reads, unless a SPICE init
  part has Qucs-S write it anew;
- what the window's Simulate on a document runs: a Python document's script, an Octave script, a data
  display's schematic.

The card lists each command and its schematic: "CMD1 runs a command in a shell after each simulation:
echo hi (amp.sch)". `allow_commands` makes no difference, since nothing shows it to the user in auto
mode. A file that is not open, which `simulate` opens first, is read from disk for the question. A batch
is asked about for all its calls.

The Simulator Settings' check of commands (`CheckCommands`, off by default) still stands behind the
guard, and sees what erc now finds too: so do Check Schematic's warnings when that check is on. No
bundled library or example has the new kinds. One bundled example has a SPICE init part with a
`codemodel` line, and it is now named.

## A2, A5. Files outside the workspace

**Inside** means the workspace, the open project, the scratch folders, Qucs-S's cache and the simulator
work folder. For reading, Qucs-S's own folders count too (examples, libraries, SPICE libraries,
documentation, Octave files, translations). The checks are made on the path as written and on the path
with its links resolved. A link the user made inside the workspace counts as inside; a relative path
with `..` that leads out does not. A folder setting that is not set (a `QDir` made empty, whose
`absolutePath()` is the program's working folder: `/` when it is started from the Finder) is no folder.

**Read**: any `path`, `file`, `against`, `compare`, `dataset`, `folder`, `symbols`, `library` or
`measured` argument that names a file or folder that exists outside. It is resolved as `absolute()`
resolves it, and also beside the document the call acts on. Also `describe_component_type`'s Verilog-A
file and `open_project`'s folder. A document open in the window is not read anew: the user opened it, or
a call that was asked about did.

**Written**, as each tool resolves it:
- `save_document`: `as`, or the document's own file;
- `close_document` with `unsaved: save`;
- `save_as` of the `export_*` tools, `get_netlist`, `import_netlist` and the synthesis tools;
- `create_subcircuit`'s `save_as`, beside the schematic when relative;
- `copy_document`'s `to`, as a folder, a project, a path or a name beside the schematic;
- `fem_model` new, import and save, and `fem_export`'s file (its node's, or beside the model);
- `import_data`'s dataset beside the schematic;
- the data display that a rename (`edit_component` with `rename`, `rename_net`) rewrites beside a schematic
  from outside.

The card says, for example: "Outside the workspace and the open project, it reads
/Users/x/.aws/credentials and writes /Users/x/Library/LaunchAgents/agent.plist. Asked about each time,
as the Permissions menu says."

## Checked again as the call runs

The question is about the call as it stands when it begins. A batch or a script can come to a use the
question never saw, for example a System Command part added and the schematic simulated, or a document
opened from outside, edited and saved. When the conversation's mode asks (the session tells the host,
`setAsking()`), the host keeps what the guards named at the call's start, and refuses what goes beyond
it:
- **each call of a batch or a script** (`unaskedWithin()`): "simulate was not made: … That was not asked
  about when the call it is part of began - make it by itself, and the user is asked first.";
- **`simulate()`** (`unaskedCommands()`), which `tune` runs: a tune that sets a System Command's text
  stops with "… was not simulated: it runs commands besides the simulator that the user was not asked
  about …";
- **the window's Simulate** (`QucsApp::slotSimulate()` → `runRefused()`), however a call sets it off.
  It covers `trigger_action`, a toolbar button through `set_ui`, F2 through `send_input`, and a tuner
  step. It applies while a call that drives the window is under way, until it answers, and while a
  call that runs alone (a batch, a script) runs. A refusal that comes after the call returned (keys
  given late) goes with the call's answer and to the status bar, with no message box;
- **every file a tool writes** (`aboutToWrite()`, and `close_document`'s save, `fem_model` and
  `fem_export`, which did not go through it): "… was not written: it is outside the workspace …".

A file written in a folder the call was asked to write in is let through. A pdf_tex's PDF beside its
LaTeX, or a copy's data display beside it, needs no second question. Where the mode asks nothing
(Bypass Permissions), and for other MCP clients, nothing is refused at run time.

The server's instructions say so in a sentence, so that Claude makes such a call by itself and not in a
batch.

## Found while reviewing the guards

I reviewed the first version by hand for ways around it. These were found and fixed above:
- **R1.** The guard (and the Simulator Settings' check) missed commands that do run: an Include Script's
  `.control` block, its included files, `source` and code-loading lines, and a `.spiceinit` in the run
  folder. A `.spiceinit` there could be written by an allowed tool (`save_document` into the scratch
  folder, which is inside) and then simulated.
- **R2.** The window's Simulate went through no guard. It was reachable by `trigger_action`, `set_ui`
  and `send_input` (F2), all unasked in Accept Edits and after "allow all". `context_menu` and
  `set_dialog` drive the window too and are treated alike. It also runs a Python document or an Octave
  script.
- **R3.** Found by the new test: the first version of that refusal, made after `send_input`'s call
  returned, opened a message box, and `send_input` waited 61 s on it.
- **R4.** A folder setting that was not set would have made the working folder count as inside.

The code model library part (`XSP_CMlib`) is not registered in this build, so it cannot be placed. Its
native code is loaded through a SPICE init part's `codemodel` line, which is counted.

## Not covered

- What a simulation writes beside a schematic from outside: its dataset and netlist, named by Qucs-S.
- A file a part names (an `.INCLUDE` or a model library outside the workspace), which the simulator
  reads. Asking would mean a question at every simulation using a model library kept outside.
- The user's own Simulate, clicked while a call that drives the window or runs alone is under way, is
  taken as the call's, and refused if it runs commands that call was not asked about.
- The tools not allowed in auto mode (`rename_file`, `trash_file`, `run_script`, git): Claude Code's
  own check sees them there. Where it asks the dock, the dock asks the user with the guards' reasons.
  `run_script`'s calls are checked as above.
- Claude Code's own tools (Read, Write, Bash): its own rules.
- A3 (`fem_export` over any file inside the workspace), A4 and A6 of the hunt are not part of this.

## Tests

- `test_qucs_control` `guardsAskAboutCommandsAndFilesOutside`. Nothing runs a command: the schematics
  have no analysis, the Simulator Settings' check is on behind the guard, and the window-Simulate case
  uses a `.spiceinit` that the fixture's ngspice (`false`) never reads. It covers:
  - the questions for both guards, file by file and tool by tool, `trigger_action` of Simulate (and a
    Python document's Run) too;
  - the run-time refusals in a batch, a script, `tune`, and the window's Simulate set off by F2;
  - what is let through: a save asked about, a pdf_tex's two files, a simulation asked about, of a file
    not open too;
  - each guard off, a mode that asks nothing, a folder setting not set;
  - a real `Session` in auto mode holding `list_documents` of an outside folder, and not in Bypass.
- `test_erc` `moreThatARunExecutesIsSaid`: the Include Script (its text, its included file, one without
  commands, one turned off), `source` and `codemodel`, the run folder's `.spiceinit` and the SPICE init
  part that replaces it.
- `test_claude_code` `guardsAreChosenUnderPermissions`: the two items, their settings, another tab and a
  new one, and the host told the guards and the mode.

The suites of the programs touched pass:
- `test_qucs_control` (209 functions);
- `test_erc` (24, 1 skipped as before);
- `test_claude_code` (68, 1 skipped);
- `test_mcp_server` (18);
- `test_git_integration` (31);
- `test_multiphysics_doc` (82);
- `test_library_export` (20).

The full-suite run found one slip of the new test's own: it expected no `SimMessage` at all, where
earlier tests had left some. It now compares before and after.

There were 41 breaks, each its own edit, built and run, then put back. 38 were caught; two of them only
once the tests were sharpened (code loading in an included file's `.control` block; the empty folder
setting, whose first fix looked at the wrong case). Three were not caught, by design:
- an open document skipped by its lookup: the same file is skipped again as open when it is resolved;
- the run-time check in `aboutToWrite()`, and in `close_document`'s save: no test call reaches either.
  A top-level call's writes are predicted by the question, and a batch's or a script's call is stopped
  first by `unaskedWithin()`. (With the batch's check broken, break 20, the batch's simulation was
  still refused, by `simulate()`'s own check.)
