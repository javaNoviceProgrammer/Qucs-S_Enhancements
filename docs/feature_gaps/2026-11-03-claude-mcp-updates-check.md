# The day's MCP changes checked live on 3 November: a model's own keys, a 3D model named, untitled documents that keep their names, a value past tera, the layout grid's words

The reviewer's live check of the day's MCP changes, on the build of 17:15 (through 25a6d4f):
`~/Desktop/qucs-reviews/2026-11-03-claude-mcp-updates-check.md`. These passed:

- the layout viewer's tools (c6d4dc8): `find_placements`, `find_shapes` 'text', `select`, `rulers`, `colours`,
  `theme`, `grid_style`, `layer_properties`, `export_image`;
- the document tools' 3D models and the 2D tools' refusal of them (fcd2457);
- `rf25_circuit` and `rf25_export` on a model opened again from its kept solve (2b81951).

Six nits were noted and the cancellation of a call (c5d190d) was left untested. All six are fixed. The round
also turned up two shipped examples with a key their node does not have, fixed too.

| Feedback | Now |
|---|---|
| **1.** "`fem_model` takes unknown top-level keys silently": `{"format": ..., "space": "3D", ...}` and `{"bogus": 1}` each made a 2D model with no word; "a misplaced `space` is exactly the case the 3D check was written for" | **Refused, nothing made.** A model given whole (`new`, `set`) has its keys checked at its top, in its component and in its results, as its nodes' properties already were. A key it does not have is named with the keys it does have, and a near name when there is one ("the model has no compnents - meant components?"). A component's key put at the top is told where it goes: "space is its component's: "components": [{"space": ...}] (2D or axisymmetric here; a 3D model is Multiphysics 3D's)". Every example, as its file has it and as the model writes itself back, is still taken |
| **2.** "`fem_edit` on a .qfem3d says only 'copy3d.qfem3d is not a multiphysics model'" | **Said to be a 3D model.** Every fem_ tool given a .qfem3d, closed or open, by its file or its tab, says "copy3d.qfem3d is a 3D multiphysics model (.qfem3d), Multiphysics 3D's: the fem_ tools act on 2D models (.qfem). It is worked in its Multiphysics 3D tab (open_document opens it there)." Other documents still get "is not a multiphysics model" |
| **3.** "`fem_model` new reported `title: "untitled (2)"`, but `close_document "untitled (2)"` said it is not open; the tab was titled 'untitled'" | **A name kept while it is open.** The cause was in the reviewer's batch: `close_document "untitled"` closed the first, and the second, numbered by its place among them, was "untitled" from that moment, so the next call's "untitled (2)" found nothing. Each untitled document now keeps the number it was first given, and a new one takes the lowest free number. `get_state` gives an "untitled (2)" its tab's own words too (`"tab": "untitled"`). A tab's words name the one tab that has them, so an "untitled (2)" left alone is still found as "untitled". A name that is no longer open lists those that are: "untitled (5) is not open; open without a file: untitled (3) (text), untitled (2) (text)" |
| **4.** "`close_document "untitled"` with two tabs of that title ... closed one of them without saying which or refusing as ambiguous" | **Says which; refuses when it cannot tell.** The close says what it closed and who keeps which name: "untitled is closed: the schematic of the tabs titled untitled. Still open, each under its name: untitled (2) (multiphysics model)." A tab's words that several documents without a file share, none of them named so, are refused, every one named: "untitled is the tab of 2 documents without a file: name one of them - untitled (3) (text), untitled (2) (text)." Where one of them is named "untitled" (get_state says which), that name is that one document's and is taken |
| **5.** "`rf25_circuit`'s 'as written': 'R 8.518e+12 TOhm' for 8.5e24 Ω - a scale prefix and an exponent together; '8.518e+24 Ohm' (or 'open') would read better" | **Both.** Past tera and below atto a value is a number alone; nought is "0 H", not "0 aH". An R past 1e15 Ohm conducts under a femtosiemens, a millionth of a femtofarad's at 1 GHz, so it is said to be an open. The reviewer's UWB filter, from its kept solve: "R 8.518e+24 Ohm (an open)". One function now writes an element for both the "as written" and the fit's lines. They also read "within 100 %" where they read "within 1e+02 %" |
| **6.** "`show_layout` wording: with `grid_style` lines it still says 'a point every 0.5 µm'; and it reported `texts: 0` drawn until `texts: true` was passed, though the file has 5 texts (possibly a tab setting left from earlier, not a bug)" | **The grid said as drawn; why no texts said; the button kept with them.** "a line every 1 µm across and up, stronger every 10 µm", "a cross every ...", "a point every ...", the stronger ones only where it draws them. The texts were the tab's Texts button, a setting kept from one session to the next, left off earlier. The answer now says so where the file has texts and none are drawn ("texts hidden": "the file's cells hold 5 texts; the tab's Texts button is off ... - 'texts': true shows them"). `texts` also set the canvas without the button, so the button showed off while texts were drawn. It now sets the button, as a click does, and is kept as the grid's settings are |
| c5d190d, cancellation: "Not tested: it needs the user to stop a turn while a long call ... is under way" | **Checked by tests and the DMG's drive** (c5d190d; `docs/feature_gaps/2026-11-03-claude-mcp-cancel.md`). To see it in the dock: ask for a long transient `fem_solve` (a model's times stretched to some thousands of steps), press Stop while it runs, then ask for `fem_stop` or `fem_describe` on the model. Nothing is under way, and the solve's progress in the tab has ended |

## 1. A model's own keys

`unknownModelKeysOf()` reads a model given as JSON the way `Model::fromJson` reads it:

- at its top: format, title, description, parameters, functions, components, studies, results;
- in each component: tag, label, unit, thickness, space, selections, geometry, materials, physics, mesh;
- in its results: plots, derived, exports.

It runs before `fromJson` in `fem_model` new and set, so nothing is made or changed when it refuses. Its lists
were checked against `Model::toObject`, which writes only keys from them, so a model read out and given back is
taken. The test holds to that for every example, read whole and then as the model writes it back.

That round trip found two examples the older node-property check refused:

- **`diode_reverse_recovery.qfem`**: its "Anode current" global graph carried `"solutions": "all"`. A global graph
  has no such property: only a line graph reads it, so the key did nothing. Taken out.
- **`point_source_wave.qfem`**: its point graph named its cut point as `"dataset"`. A point graph reads `"data"`,
  so the graph had no dataset. Now `"data": "cpt1"`.

## 3 and 4. Untitled documents' names

`titleOf()` gave a document without a file its place among those whose tab reads the same: the second
"untitled", "untitled (2)". A place changes when one before it closes. The reviewer's batch closed "untitled" and
then "untitled (2)", and the second call found nothing, because the second document had become "untitled" in
between.

Each such document now carries its number on its tab's widget, given the first time any of them is named. The
documents not yet numbered take the lowest free numbers, in tab order. A number is kept while the tab's words
stay the same and the document has no file. A document saved under a name is named by its file, as before.

Measured through `qucs-s --mcp-server`:

- **The reviewer's batch**: "untitled is closed: the schematic of the tabs titled untitled. Still open, each
  under its name: untitled (2) (multiphysics model)." Then "untitled (2) is closed."
- **Three made, the middle one closed**: the others keep "untitled" and "untitled (3)". The next one made is
  "untitled (2)".
- **"untitled (3)" left alone**: `close_document "untitled"` closes it, the one tab saying "untitled".
- **"untitled (2)" and "untitled (3)", neither named "untitled"**: "untitled" is refused, both named.

## 5. Values past the prefixes

`prefixed()` took the largest prefix a value reached, so 8.5e24 Ohm was 8.518e12 times tera. Past 999.95 T and
below 1 a, and at nought, it now writes the number alone.

The reviewer's R came from the fit, not the solve. The UWB filter is distributed, so no branch between its ports
fits (its own misfit 1). R's least squares then drives the conductance towards nought, and the search stops where
its steps no longer help. The test builds that case directly: a susceptance between the ports that turns its sign
at every frequency, so that R, L and C all fit badly. The fitted R is then 65.6/b, 2.6e15 Ohm for 2.5e-8 S,
for any b. The test holds the fit to an R past 1e15 Ohm, written as a number and "Ohm (an open)", with no prefix
beside an exponent and "within 100 %".

## 6. The layout grid and texts

`gridSaid()` writes the grid as `LayoutView::paintGrid()` draws it:

- lines run across and up at every step;
- crosses and points sit at every crossing;
- the stronger ones fall on every power of ten, which `paintGrid` leaves out when the step is the database unit
  and not 1, 2 or 5 of a power of ten. The words leave them out there too.

The texts: the tab's Texts button is `LayoutViewer/labels` in the settings, read when a tab opens. The reviewer's
was off from an earlier session. `show_layout` with `texts` set the canvas alone (`LayoutView::setLabelsShown`),
leaving the button as it was. `LayoutDoc::setLabelsShown()` now checks the button, and the button's toggle draws
and keeps it, as a click does.

## Tests

- `test_multiphysics_doc`:
  - `aModelsUnknownKeysAreRefused`: the reviewer's two models and a typo at each level refused, nothing made; set
    refused, nothing to undo; every example taken whole and as written back.
  - `aThreeDModelsFileIsSaidToBeOne`: fem_edit, fem_describe and fem_solve on a .qfem3d by its file, closed and
    open, and by its tab; a text document still "not a multiphysics model".
- `test_qucs_control`:
  - `untitledDocumentsKeepTheirNames`: the reviewer's batch, the tab words, the lowest free number, the ambiguous
    refusal, a name no longer open.
  - `anRf25ModelIsSolvedFullWaveThroughItsTool`: each branch's "as written" as `elementSaid()` writes its values.
- `test_rf25_sparams`:
  - `theSpiceSubcircuitIsTheCircuit`: values past the prefixes, nought, `isOpenInEffect()`, `elementSaid()`.
  - `anOpenBetweenThePortsIsWrittenPlainly`: the case above.
  - `aLongLineIsSaidToBeNoLumpedCircuit`: misfits past 10 % written without an exponent.
- `test_layout_doc`:
  - `show_layoutColoursTheLayersAndTheCanvas`: the grid's words for lines and crosses.
  - `show_layoutShowsACellRegionAndLayers`: `texts` sets the button, kept in the settings, "texts hidden" said
    and gone.

## Breaks

`2026-11-03-claude-mcp-updates-check/breaks.py` undoes each fix in turn, builds, and runs its tests:

- **fem_model's keys** (10): each check (new, set, the component's, the results'), a component's key at the top,
  the near name, a key taken out of each list, and each example's key put back.
- **A 3D model to the fem_ tools** (3): its file closed, the tab open, and every other document called one.
- **Untitled names** (7): the number its place again, a new one past the others, get_state without the tab's
  words, the close not saying which, a tab's words not taken, the first of several taken, a name gone with
  nothing said.
- **Values** (7): past tera, below atto and nought, no R an open, `elementSaid()` without it, the fit's lines and
  the tool's "as written" each writing their values as before, 100 % as "1e+02 %".
- **show_layout** (3): the grid "a point every" of every style, texts hidden not said, `texts` the canvas's alone.

All thirty were caught. One of the first thirty passed: putting nought back to "0 aH" by taking out `a == 0`.
Nought was already below the atto bound, so the test of nought was redundant. It is gone, and its break now takes
out the bound itself. Rebuilt after the breaks.

Then the full ctest (159), the MCP scenario script (97 checks) and the four lints passed, with nothing written to
the real cache.

## Under the sanitizers

Built Debug with ASan and UBSan, as CI's Linux job is, each function alone. None reported anything:

| Function | Time |
|---|---|
| test_multiphysics_doc `aModelsUnknownKeysAreRefused` (49 examples set twice) | 6 s |
| test_multiphysics_doc `aThreeDModelsFileIsSaidToBeOne` | 1 s |
| test_qucs_control `untitledDocumentsKeepTheirNames` | 5 s |
| test_qucs_control `anRf25ModelIsSolvedFullWaveThroughItsTool` | 29 s |
| test_layout_doc `show_layoutShowsACellRegionAndLayers` | 5 s |
| test_layout_doc `show_layoutColoursTheLayersAndTheCanvas` | 3 s |
| test_rf25_sparams, the three functions | 1 to 4 s |

## Not done

- **A name the tools give an untitled document is not on its tab.** The tab of an "untitled (2)" still reads
  "untitled", as Qucs-S has always titled a new document. `get_state` gives both, and a tab's words are taken
  where they name one document. Retitling the tabs themselves would change what the user sees of every new
  document, and was not asked for.
