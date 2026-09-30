# Feature gaps: Qucs-S's MCP tools, the tenth round of feedback

*30 September 2026 — Qucs-S 26.1.4.*

Claude verified `343cc16`, the answer to the wishlist of the morning
(`qucs-s-mcp-round10-verification-2026-09-30.md`). It checked the installed
app with the window open, and the scenario script against the same binary:
s1 to s9, 71 checks, none failed. It judged all seven items delivered, and
found two faults. Both were reproduced through `qucs-s --mcp-server` in a
project of their own before they were changed.

## The findings

| Feedback | Now |
|---|---|
| **A. `add_diagram` previewed onto a data display that does not exist leaks a file.** `document: "data_display"` with `preview: true` said `would change: []`. Yet it left `ua741_noninv.dpl` open with unsaved changes and a 0-byte file on disk. | Reproduced: `amp.dpl` open and changed, 0 bytes on disk. The preview put back the schematics open when it began; the data display was not one of them. Making a data display opens it (as Qucs-S's own Change to Data Display does) and writes its empty file at once. Now a preview notes the documents open before it. Afterwards it closes, unchanged, any the call opened, and a file made for a data display is taken away with the files a preview puts back. `would change` says so: `{"document": "amp.dpl", "changes": ["made (it had none), and opened", "diagram 1 added (Rect)"]}`. A data display that exists but is not open is opened for the preview and closed after, its file byte for byte as it was ("opened"). The wrong-looking file is harmless in itself: a 0-byte `.dpl` loads as an empty data display. |
| **B. A Scratch folder inside the Scratch folder for an untitled document.** | Reproduced. An untitled schematic is saved as `Scratch/untitled.sch` to be simulated, and its own scratch folder was taken from its path in the project: `Scratch/Scratch/untitled`. Now `misc::scratchDirFor` takes a schematic in the Scratch folder by its place there: `Scratch/untitled`, beside the file the run saved. |
| (B, second part) **A schematic outside the project ran in the project's Scratch** (`/tmp/vatest/rc_va_test.sch` in `opamp741_prj/Scratch/rc_va_test`). | Reproduced: `Scratch/foreign`. A schematic outside the open project now writes nothing into it. It gets a folder of its own where schematics of no project go (`S4Q_workdir/foreign-<key>`), named by it and told apart by its folder, so two `x.sch` of two folders do not share one. |
| (found on the way) **`clean_scratch` trashed the scratch folder that schematics of no project share.** | With no project open, every schematic runs in one flat folder (`S4Q_workdir`). `clean_scratch` meant to clear only a schematic's own subfolder ("never the folder shared"), but told the shared one only by the name `Scratch`, and this one is `spice4qucs`: it went to the trash whole. Now, in the shared folder it clears the files of the last run there when that run was this schematic's (the netlist's first line names it). It leaves the folder and anything else in it. When the last run was another schematic's, it clears nothing, and says so. |

## What was not verified live

Each of these is covered by a unit test or a scenario that runs headless.

| Not verified live | Where it is checked |
|---|---|
| `set_subcircuit_parameters` without preview; instances following on save | `subcircuitParametersAreSetAndFollowByName`: set, removed, replaced; an instance's values by name after its subcircuit is saved; the `.SUBCKT` and X lines of the netlist. |
| The `hint` on a Verilog-A module that is not found | `aModuleNotLoadedIsExplained`, with ngspice: nowhere, then in a subfolder of the workspace. |
| Staleness of a copy, and after `tune apply=false` | `aStaleDatasetIsSaid`, with ngspice. |
| Dialogs in the visible window | s9, headless; not in a visible window. |
| `replace_component`'s refusal and pin map | `unnamedPinsOntoNamedOnesAreGuessed`: the OpAmp turned and not onto the uA741, ua741(TI) by number, a turned resistor onto a subcircuit. |

## Tests

- **`test_qucs_control`**, new: `roundTensFindings`. It previews a data display that does not exist: `would change` names it, made and with its diagram, and afterwards no file and nothing open. It previews one that exists and is closed: it is opened, closed after, and its file is unchanged. With ngspice and a project, it simulates an untitled schematic: `Scratch/untitled`, no `Scratch/Scratch`. It simulates one from outside: nothing of it in the project, and its netlist in its own folder. Changed: `projectsAndCopiesAreTended`. Its `clean_scratch` of a schematic of no project now runs in the shared folder: another's run is left and said, its own is cleared, and the folder and a subfolder stay.
- **`test_project_scratch`**: a schematic outside the project goes where schematics of no project go, another of its name to another folder. One in the Scratch folder is named by its place there.
- **`scripts/mcp-e2e-scenarios.py`**: s1 to s9, 71 checks, none failed.

Each change was broken on purpose, seven breaks in all, and each is caught:
the documents a preview opened left open, the file made for them left on
disk, `Scratch/Scratch` (by both tests), a foreign schematic in the
project, `clean_scratch` trashing the shared folder, and clearing another
schematic's run.

The full suite passes (79). `~/QucsWorkspace` and `~/Library/Caches` are
as they were before the runs (checked against a marker file).

The tools sent every turn are as they were.
