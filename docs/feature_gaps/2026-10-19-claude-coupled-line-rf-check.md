# Coupled lines: the check of 31ba73b, its nits and the tools' (answered)

The reviewer's file: `~/Desktop/qucs-reviews/2026-10-19-claude-coupled-line-rf-check.md`. It checked the coupled-line RF wishlist's answer (31ba73b; [its write-up](2026-10-18-claude-coupled-line-rf-wishlist.md)) in the open app, build f847d77, on a GSSG pair (`gssg_coupled.qfem`) and its odd half-model. An addendum checked the names offered as an expression is typed (f847d77; [its write-up](2026-10-19-claude-expression-completion.md)).

**The verdict:** all six items pass, and the completion list passes by eye and in its test. There were two nits on the coupled lines and one remark. The addendum found four nits in the tools that drive the panel.

The coupled-line nits were reproduced first, over `qucs-s --mcp-server` with an isolated home and cache, on a copy of the reviewer's model. The tools' were reproduced headless on the same model (the tree's) or by the tests below, broken back to the old code (the others).

## The coupled lines

| Feedback | Now |
|---|---|
| **1. `fem_evaluate` cannot read a derived node's values.** `lp1.Z0_odd` (and `lp1.R_1_1`, with or without `all`) failed with "'lp1.Z0_odd' is not a parameter, a variable or a function here", while `fem_solve` said "fem_evaluate with 'all' gives each" of the node's 120 values. | **`fem_evaluate` takes a derived node's tag**, as the Global graph already did: `lp1` gives each of its values, `lp1.Z0_odd` one, and they mix with expressions (`z = lp1.Z0; es.C11`). With `all`, each solution's, named by it. With no `study`, the node's own. `unit` converts them. A value it does not have is refused with the ones it has ("<node> has no value nothing: it has C_1_1, ..."). **`fem_solve`'s note** names it: "120 values: the last 40 here; fem_evaluate lp1 with 'all' gives each (lp1.<name> one)". **On the reviewer's model:** `lp1.Z0_odd` 72.04 Ω − j1.972 Ω at 50 GHz, as the node shows; `lp1.R_1_1` with `all` 2.953, 4.636 and 7.982 kΩ/m at 1, 10 and 50 GHz. |
| **2. A freshly written 1D dataset flagged stale**: "gssg_coupled_pg1d2.dpl was edited at 11:05:48, after this dataset was written (11:05:48)". | **Reproduced:** "gssg_coupled_pg1d2.dpl was edited at 21:25:11, after this dataset was written (21:25:11)". `get_dataset` judged a dataset by the open schematic that names it, here the plot's data display: `fem_plot` writes the dataset, then puts its diagrams on the display, a few milliseconds later. Two things were wrong. A display's edits (diagrams, markers) never change its data. And a model's dataset is of a solution, not of a circuit. **Now:** a dataset a model wrote (its origin names the `.qfem`) is judged by the model. It is stale when the model changed since the group's study was solved ("coax_rlgc.qfem changed since Study was solved: this dataset (written at ...) is of the model as it was - fem_solve, then fem_plot pg1d1, writes it anew"). Or when the study was solved again after it was written ("... was solved again at ..., after this dataset was written"). With the model not open, when the model was saved after it. A circuit's dataset read through its data display is judged by the circuit's open schematic, never by the display. **The reviewer's dataset** now has no "stale". |
| **Remark: the mixed-mode file's option line reads `R 50`**, while the header gives the modes' references as 100 and 25 Ω. | **Said in the header**, under the references: "(The option line's R 50 is the single-ended reference the modes' are of - twice it, half it: Touchstone 1 has one reference)". |

To check the stale one, `MultiphysicsDoc` now keeps when each study was last solved (`solvedAt`).

## The tools on the panel (the addendum)

| Feedback | Now |
|---|---|
| **1. `send_input keys "Ctrl+A"` on a dock field ran the window's Select All**, so the text typed after it was appended (`es.V2*es.`). | **The field has the first claim on its keys, as on the keyboard.** Before a key, `send_input` sends the ShortcutOverride event a keypress brings. A field that claims the key gets it: a line edit claims Select All, Undo, Copy, the arrows. Otherwise the window's shortcut fires. The answer says which: "keys ⌘A: the field's". |
| **2. `send_input` cannot drive a completion popup; the list is invisible to `get_ui` and the pictures.** Wanted: keys to the popup, or `get_ui` listing its rows and `set_ui` taking a row. | **Both.** **The keys go to the open list**, as a keyboard's do (it has the grab): Up and Down move, Return and Tab take the row, Escape closes it ("keys ↓: in the list, ↵: in the list"). Once it closes, the field has the focus again, and typing goes on in it (the list opens again after the next name and dot). **The answer carries the list** while it is open: its rows (the whole name, its unit, what it is), the row chosen, the field it is of, and the field's text. **`get_ui` area `list`** reads it, and `get_ui`'s areas list it while it is open. **`set_ui` area `list`** takes a row by its number or its name (`"set": [{"control": "row", "value": "normE"}]`): "took es.normE", with the field's text. **The pictures show it:** `send_input`'s picture of the dock has the list drawn below its field, and `screenshot` of the window adds the list's picture ("Pictures of the window, 1202 x 533, the list open below Expression"). |
| **3. `set_ui` on the panel's Expression field did not commit**: "Set: Expression", and the node kept its value. | **A field set is done with, as a click away does.** After its text, a field that takes the value gets `editingFinished`, and the panel commits it. The reviewer's case: the node's expression became `es.V*2`. |
| **4. `set_ui` on the tree (mpTree) refused a row number and a path**: "a row is [row, column, true or false (checked), or a text]". | **A tree takes a bare row**: a number as `get_ui` lists the rows (every row, the collapsed ones too), a name, or a path "A > B > C" from the top or from any row that matches its first part. A row and its column (`[row, column, value]`) still set a cell. The answer names the row by its path. The reviewer's cases: 45 is "gssg_coupled.qfem > Results > Electric Potential (es) > Surface", and "Results > Electric Potential (es0) > Surface" is the es0 group's. |

**Found on the way: the list closed at the next key, and typing after it reached no field.** A completer hides its list at a key it hands on to its field (a letter) while that field has not the focus, and a field has none in a window that is not the active one. That is the case when the user is in another window, and offscreen once a list has closed: the popup took the activation, and its going leaves no window active.
- **The keys wait for the window.** `send_input` activates the target's window before its keys (unless a dialog or a list is the target). The activation is a window-system event still to come, so the keys go once the window is active, or half a second on without it.
- **The field keeps the focus as its list closes.** A key into the list that closes it, or `set_ui` taking a row, activates the field's window again and gives the field the focus, so the keyboard's next key is the field's.
- **A key with no focus anywhere** goes to the field the window keeps its focus on.
- **Since: a character typed with the list open goes to its field**, as the list hands a letter on, and the field's completer narrows the list. Through the list, the list closed at it whenever the field had not Qt's focus. That happened on CI's macOS runner (b5acf84's run: the window's activation had not come yet), and it would on a Mac with Qucs-S behind another program, where it never comes. The panel test now takes the field's focus away with its list open and types on, and the other window in front has a field with the focus, as a real one does. The arrows, Return, Tab and Escape still go to the list.

## Tests

- **`test_multiphysics_doc`:**
  - `derivedValuesEvaluatedAndTheirDataset` (new), on the coax example's line parameters:
    - `lp1.R` in Ω/m, with `all` each frequency's, the last the single one;
    - `lp1` at 100 kHz, all its values;
    - `z = lp1.Z0; es.C11`, named;
    - a value the node does not have, refused;
    - its 1D group's dataset not stale just after `fem_plot`;
    - stale after the model's parameters change, and after the study is solved again.
  - `thePanelThroughTheTools` (new):
    - the tree's row by `get_ui`'s number, and by paths from the top and from inside;
    - the Expression field committed by `set_ui`;
    - Ctrl+A the field's, with a window-wide Ctrl+A action that must not fire;
    - the list in `send_input`'s answer, `get_ui`'s and the window screenshot's;
    - Down and Return take the second row;
    - typing goes on and narrows the list;
    - `set_ui` takes a row by name, and the list is gone after it.
- **`test_qucs_control`:** `aStaleDatasetIsSaid`, a circuit's dataset read through its data display: not stale for the display's own edits.
- **`test_fem_magnetics`:** `aCoupledPair`, the mixed-mode header's note on the option line.

The panel test makes another window the active one, as when the user is in another window, before its keys. It types a letter after the dot while that window is still in front, and it checks the focus is the field's after the list closes.

**Found here, fixed in the change after: a new document's first edit was not recorded.** `QucsDoc::edited()` took revision 0 to 1 as the content's loading, and recorded no edit. An opened file gets revision 1 at loading, but a new document starts at 0, so its first edit was lost: `get_state` showed no "last edit" after the first part added to a new schematic, and the changes-since report missed it too. A sheet lost the first edit after it was opened as well, and did not count being read again. Now `edited()` records every edit, and a document's loading says itself (`contentRead()`: the first read is revision 1, no one's edit; a later one is an edit). The tests `aNewDocumentsFirstEditIsCounted` and `aModelsFirstEditIsCounted`; six changes broken on purpose, all caught.

Twenty changes were broken on purpose, and nineteen were caught:

| Break | Caught by |
|---|---|
| S1 a derived tag not taken | derivedValuesEvaluatedAndTheirDataset ("'lp1.R' is not a parameter") |
| S2 a model's dataset judged by its display | derivedValuesEvaluatedAndTheirDataset (no "changed since" after the model's edit) |
| S3 a display's own edits counted | aStaleDatasetIsSaid (once the display's edit was on record: see the first-edit note above) |
| S4 no solve time kept | derivedValuesEvaluatedAndTheirDataset (no "solved again") |
| S5 the window's shortcut before the field's | thePanelThroughTheTools ("the field's") |
| S6 keys not to the open list | thePanelThroughTheTools |
| S7 the open list not in `send_input`'s answer | thePanelThroughTheTools |
| S8 the window not activated for keys | thePanelThroughTheTools (the letter after the dot closed the list; once the other window was in front) |
| S9 no fallback to the window's focus field | **not caught.** Offscreen, the activation always comes, and the closing list gives the field its focus back, so a focus widget is always there. The fallback is for an activation that does not come, as a window system may refuse to bring an app forward from behind another program: the keys then go after the half second, with no focus widget |
| S10 `get_ui` without the list area | thePanelThroughTheTools |
| S11 `set_ui` without the list area | thePanelThroughTheTools |
| S12 a panel's field not done with | thePanelThroughTheTools (the node kept its expression) |
| S13 a tree's bare row refused | thePanelThroughTheTools |
| S14 a tree widget's rows numbered as shown | thePanelThroughTheTools (another row chosen) |
| S15 a path from the top only | thePanelThroughTheTools |
| S16 the window's screenshot without the list | thePanelThroughTheTools |
| S17 the option line's reference unsaid | aCoupledPair |
| S18 the field's focus not kept as its list closes | thePanelThroughTheTools (no widget had the focus) |
| S19 `set_ui`'s row: the field's focus not kept | thePanelThroughTheTools |
| S20 the keys not waiting for the window's activation | thePanelThroughTheTools (the list closed, as S8) |

The first run had three misses: S3, S8 and S18. The tests were made to reach them as above. Two of them showed the fixes were short:
- With another window in front, the activation before the keys came after the keys. Now the keys wait for it.
- The restored focus was no focus at all in a window not active. Now its window is activated too.

## Runs

- **Release:** the changed functions pass, and the Release suite 131 of 131.
- **The scenarios** (`scripts/mcp-e2e-scenarios.py`, the build's app): 97 checks, none failed.
- **The reviewer's flows over `qucs-s --mcp-server`**, on the final build and a copy of the reviewer's model, all as in the tables above.
- **Sanitizers** (ASan and UBSan), with no report:
  - `test_mcp_server` whole;
  - `aCoupledPair`;
  - the two new `test_multiphysics_doc` functions and `namesOfferedAsTyped`;
  - the `test_qucs_control` functions of the tools changed: the window's and panels' controls, `send_input`, the stale, imported and exported datasets.
- **An older order dependence:** in that sanitized run, `aDataFileIsImportedAndPlotted` failed after `theResultsAreReadAndPlotted`, its neighbour in the subset. It fails in that order in Release too, and on the code before this round. Its `reload_data` check read the first document reloaded, which another test left open with its diagrams. It passes alone, sanitized too, and in the suite's order. Fixed in a change after: it reads its own schematic's entry, by name.
