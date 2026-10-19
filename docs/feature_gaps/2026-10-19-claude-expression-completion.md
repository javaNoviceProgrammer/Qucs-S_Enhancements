# Names offered as an expression is typed

The user (2026-10-19), with a screenshot of a surface plot's Expression box holding `emw.`: *"Do you think you can add autocomplete to the physics outputs when dot operator is being used. i.e., if I type "emw." in a box then a list of variables that are associated with the emw physics is shown so that I can quickly know what is available for me to use."*

## What it does

In the Multiphysics panel's expression boxes (`multiphysicspanel.cpp`, `ExpressionEdit`):

- **After a name and a dot** (`emw.`, `semi.`, `ht.`), a list opens below the box with every name that goes on from it. Each row shows the rest of the name (`Ex`, `normE`, `Sz`), its unit (V/m) and what it is ("electric field, x component..."). A long description is cut at the list's width; the row's tooltip has all of it.
- **Narrowed as one types:** `emw.no` leaves `normE` and the other names that start with "no", in any case. Typing past the word (an operator, a space) closes the list, and so does moving the cursor off it.
- **Keys:** the first row is chosen. Up and Down move, and Return or Tab puts the name in place of the word. Escape closes the list. A click takes a row too.
- **Asked for:** Ctrl+Space (the Control key on a Mac, the Command key's being Spotlight's) lists every name that starts with the word at the cursor, or every name when there is none. The parameters are among them, with their values. The box takes the key before any window shortcut of it.
- **Only as text is added:** typing or pasting opens the list; deleting does not. Backspacing into `emw.normE` and pressing Return finishes the box, as before.
- **The box is committed when it is finished**, not while the list is open. The list's Return takes the name; a second Return commits.

What each box offers:

| Box | The fields' variables | The globals | The parameters |
|---|---|---|---|
| A spatial expression: a plot's, a derived value's, a physics feature's | yes | in a result's (plot, graph, derived value, dataset, export) | yes |
| A Global Evaluation's Expressions, a global graph's Expressions and its "Against a global" | no (they are of the whole model) | yes | yes |
| Any other expression (a material's, a size) | no | no | Ctrl+Space |

- **The variables** are the model's (`modelVariables`: what a solution of it would have, before any solve). Where a study is solved and up to date, the solution's own come too, where it has more (a laser's).
- **The globals** exist once a study is solved. In the list each shows its value where its study has one solution (one point, one time, one mode), as `emw.neff = 2.445`. Where it has several, the list shows its unit: the last solution's value would be one of its modes, points or times. The SOI example's last is its TM0 mode (1.77), while its plot shows TE0 (2.446).
- **A number's dot** (`1.5`) offers nothing: no name starts with a digit.

## Tests

`test_multiphysics_doc` `namesOfferedAsTyped` (new) uses the SOI waveguide example. It solves one wavelength on a coarse mesh: 385 triangles, neff 2.439. On the example's fine mesh the two solves took 234 s under the sanitizers.

1. **The surface plot:** it types `2*emw.` and checks:
   - the list holds Ex, Ey, Ez, normE and Sz, shown without the stem, and not `x`;
   - normE's unit is V/m and it has a description;
   - the list is at least as wide as the box, and its first row is chosen;
   - the node is not changed while the list is open.
2. **Narrowing and taking:** `no` narrows the list to names starting with "no". Return takes normE, and the text is `2*emw.normE` with the node still unchanged.
3. **No list:** `+1.` opens none. Three backspaces back into the name open none either.
4. **Commit:** Return commits `2*emw.normE`.
5. **Solved, one mode:** the Global Evaluation's `; emw.n` lists `neff = 2.4...`. Escape closes the list.
6. **Ctrl+Space:** a window shortcut of the same keys, added by the test, does not fire. The list holds `emw.neff` and the parameter `w = 500 nm` ("the wire's width"), but not `emw.normE` or `x`.
7. **Two modes** (`fem_edit`, then solved again): no global shows a value, and `emw.loss` shows dB/m.
8. **The surface again:** its `emw.` list holds both normE and neff.

Eleven changes were broken on purpose. One was dropped and one is caught only on the display:

| Break | Caught |
|---|---|
| B1 Return goes to the box while the list is open | yes |
| B2 deleting opens the list | yes |
| B3 full names shown, not after the stem | yes |
| B4 a value shown for a study of several solutions | yes |
| B5 the global expressions plain boxes | yes |
| B6 a number's dot refused by its own test | redundant: no name starts with a digit. The test is dropped from the code; `+1.` is still checked |
| B7 a point's names offered in a Global Evaluation | yes |
| B8 no globals in a result's box | yes |
| B9 no parameters | yes |
| B10 Ctrl+Space not taken from a window shortcut | on the display (Cocoa) only. Offscreen the test's window is never active, so no shortcut matches. With a model in front, the app's own Ctrl+Space actions (the Python completions, a VHDL entity) are disabled, so this is a guard. |
| B11 the first row not chosen | yes, after the test was sharpened |

## Runs

- **Release, offscreen:** `namesOfferedAsTyped` passes in 1.1 s. The whole of `test_multiphysics_doc` passed, 61 of 61.
- **Release, on the Cocoa display:** it passes, with a real popup window and keyboard focus. The box was not committed while the list was open.
- **Sanitizers** (ASan and UBSan, Eigen's LU): it passes in 74 s with no report.
- **The Release suite:** 130 of 130.
