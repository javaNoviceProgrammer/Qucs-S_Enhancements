# Feature gaps: Claude's full control of Qucs-S, the check of 3 October

*5 October 2026 - Qucs-S 26.1.5.*

The check (`Qucs-S_control_check_2026-10-03.md`) drove Qucs-S at `935b184`
through its tools, with ngspice 46. It built an RC low-pass (V1, R1 1k, C1 159n) from
an empty schematic, then simulated, measured, tuned, plotted and exported it, and it
used the menus, dialogs, docks and consoles.

Its verdict: Claude can drive nearly all of Qucs-S. It found one real bug in a core
path - a part's value changed in its Properties dialog appeared to apply but was never
written - and six smaller gaps.

All seven were reproduced on `ce7216a`, as written, through `qucs-s --mcp-server`
with its own HOME, settings and workspace:
- **Item 1:** a value set in the dialog's field (c4) or in its table cell showed
  in the dialog, and after OK the netlist still said `1K`. The Symbol list in the
  same table did apply.
- **Item 5:** the answers were "Set: ." and "Set: ohmic resistance in Ohms".
- **Item 3:** after a run, the variables table read `[[false, false, false],
  [false, false, false]]`.

## What was done

| Feedback | Now |
|---|---|
| **1. Component Properties: set_dialog's value edits are never applied (HIGH).** | **Applied.** The dialog edits each value in a field that the property table keeps open over the cell, its editor. Only the editor's text was set, so the table item kept the old text, and OK wrote that back. Now a field that is a table cell's editor gives its value to the cell, as the cell's delegate does when the user presses Return or leaves the field (`commitEditor` in `qucscontrol.cpp`). This holds whether the field is set by its id, by its label, or as the table's cell `[0, 1, "2.2k"]`. The lists in the cells, such as the Symbol, are widgets their dialogs read themselves, as before. |
| **2. add_marker -3dB refuses a trace drawn in dB (MEDIUM).** | **Placed.** A trace with `part: db` holds dB values once read, but the search asked only whether the *variable* was in dB. Now a trace drawn in dB is in dB: on the RC low-pass the crossing is 998.7 Hz, the level -3.000 dB, and the marker sits on the sample at 1 kHz. A trace showing a phase, a real or an imaginary part is refused with the reason: "Trace phase(ngspice/ac.v(out)) shows its phase: 3 dB below its peak is of a magnitude or of dB - its 'part' magnitude or db (edit_trace), or another trace of ngspice/ac.v(out) so." A magnitude is measured as before (the peak over √2). |
| **3. get_dialog cannot read Diagram Properties' variable tables (MEDIUM).** | **Read; and a trace is chosen there.** The dialog makes its cells read-only with `flags() ^ ItemIsEditable`. A new table item is user-checkable from the start, so the cells kept that flag although no check box is shown, and `get_dialog` read each as an unticked box. Now an item counts as a check box only when it has a check state, in tables, lists and trees alike. The variables read `["ac.v(out)", "dep", "frequency"]`, the graphs `["dB(ngspice/ac.v(out))", "", "solid", "1", "left"]`. For the impact the check names, `set_dialog` now takes `action` (select, or activate - a double click) on a dialog's table, list or tree row, as it does in a panel. Activating `ac.v(out)` in the variables table adds the trace, as the user's double click does. `set_dialog` also no longer sets a check box on such a cell: a cell with a box takes true or false, a cell with text takes text when it can be edited, and otherwise the answer says so and points to `action`. |
| **4. get_settings scope document leaves out the Frame tab (LOW).** | **Read and set.** The Frame tab's five controls had no labels, and a setting is keyed by its label. They are now named: `Frame/Size`, `Frame/Title`, `Frame/Drawn by`, `Frame/Date`, `Frame/Revision`. The names are accessible names, so a screen reader has them too. `set_settings` changes them, with each change's `was`. |
| **5. set_dialog's summary is blank for controls without a label (LOW).** | **Named.** A field or list in a table's cell is named by its row and column - "R (Value)", "Tc1 (Value)", "Symbol (Value)" - in `get_dialog` and in the answer, and it can be found by that label (`"control": "R"`). A table cell set is said with its row and column: "c3's cell 0, 1 (R Value)". A control with no label at all is named by its id. A cell's button keeps its own text, so every `set_settings` key is as it was. |
| **6. send_input cannot reach an open dialog (LOW).** | **`target: "dialog"`.** The dialog that waits for an answer takes clicks in its picture's pixels, with each control's `at` given by `get_dialog`. Keys and text go to its field with the focus, which a click gives. The window's shortcuts are not set off: keys reach only the dialog. It is the one input besides `set_dialog` that gets past the gate while a dialog waits; every other target still waits. The answer is a picture of the dialog, or "closed: it was answered". Typed into R1's field then Return, the value is in the cell, and OK applies it. Typed then OK clicked, it is applied as well, since leaving the field commits it. |
| **7. add_analysis does not take `properties` (ergonomics).** | **Taken**, as `add_component` takes them, over the defaults of `kind`: `{"kind": "ac", "properties": {"Start": "10 Hz", "Stop": "100 kHz", "Points": "201"}}`, or a transient's `MaxStep`. The same thing given twice, as `from` and as `Start`, is refused ("'from' and 'properties' Start say the same: give one of them. Nothing was added."). An unknown property is refused with the analysis's list, as in `add_component`. |

The tools' descriptions say this: `get_dialog` (its `at`, a cell's label),
`set_dialog` (a cell's editor, `action`, the names in its answer), `send_input`
(target dialog) and `add_analysis` (`properties`).

## Tests

`test_qucs_control`, three new tests at the end of the class:
- **`aPropertyDialogsValuesAreApplied`** (items 1, 5 and 6), in Edit Component
  Properties:
  - the field set by its id and by its label, and the table's cell: each applied, its
    answer naming it;
  - a cell's check box (Show) set; a text given to it, refused with the reason;
  - `send_input` into the dialog: click, Ctrl+A, typing, Return, then OK; and typing
    then a click on OK;
  - `target` dialog with no dialog, refused; the canvas while the dialog waits, still
    refused.
- **`aDbTracesMarkerAndTheDiagramDialogsTables`** (items 2 and 3):
  - a 1 kHz first-order low-pass as a dataset: -3dB on its dB trace at 1 kHz ± 20 Hz;
  - the phase refused; the magnitude measured on a magnitude;
  - Diagram Properties: both tables read as text; a variable activated adds a second
    graph, which OK keeps;
  - a read-only cell set as text: refused, pointing to `action`.
- **`theFrameAndAnAnalysissPropertiesAreSet`** (items 4 and 7): the five Frame keys,
  set and found on the schematic; `add_analysis` with `properties` for AC and for a
  transient's `MaxStep`; the same setting twice, refused; an unknown property, refused;
  nothing added by a refusal.

- **`itemsWithoutCheckBoxesAreReadAsText`** (items 3 and 5, any dialog):
  - a dialog's plain tree and list: their items read as text, with no `checked`; a ticked
    item read as ticked;
  - a subcircuit's File: the field in the cell is "File (Value)", is set, and is applied;
    the cell's button keeps "...".

`scripts/mcp-e2e-scenarios.py` **s13**, the check's own RC low-pass, with ngspice:
- `add_analysis` with `properties`;
- the bandwidth at 1/(2πRC); R1 set to 2k through `set_dialog` by its label, and the
  bandwidth halves; set back to 1k through `send_input` into the dialog, and it comes
  back;
- the -3dB marker on the dB trace at that bandwidth;
- Diagram Properties read, and a variable activated;
- the Frame tab read and set.

Each fix was broken on purpose: 23 breaks, all caught. The first run left four uncaught:
- **A button in a cell keeping its name, and lists and trees without check boxes.** No
  test reached them: the new dialogs have none. `itemsWithoutCheckBoxesAreReadAsText`
  now does.
- **Keys sent to the dialog's own focus field rather than the application's.** It made
  no difference: Return reaches the field either way, through `set_dialog` and through
  `send_input`, in the MCP server as in the tests. My first probe seemed to need it, but
  it sent Escape after Return, which cancels the dialog whatever was committed. The line
  was taken out.

Also left out, as making no difference: the commit on `set_settings`'s path (no
settings dialog has a cell editor), and a separate rule for a dB variable shown as a
magnitude.

Full suite 88/88. Under AddressSanitizer 88/88, with no report;
`buildErrorsAreMarkedInTheTab` once found OpenVAF not starting while four tests ran at
once, and passed alone. The end-to-end scenarios, s1 to s13: 97 checks, none failed.

## Not done

- **The things the check left untested** - `set_settings` on the user's own settings,
  the Octave and Terminal consoles, Print and Open - are as they were. trigger_action
  still refuses Print, Open and Save As by design: their dialogs are the user's.
- **A table cell that is neither editable nor a check box** (Diagram Properties'
  variables) is not given a text. Its row is chosen with `action`, as a click chooses it.
