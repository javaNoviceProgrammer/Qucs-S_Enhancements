# The eight findings of a bend-loss study

The user ran a bend-loss and mode study of their own in another session and wrote down eight things
Qucs-S made them work around: a dataset they could not use, a value that is not there, a parameter
that cannot be swept, a property taken in silence, a folder that is not made, plots gone blank, axis
labels that say the wrong number, legend samples that say the wrong style. Seven are fixed with a
test that fails without the fix; one (6) does not reproduce at this commit, and what was tried is
written down here rather than a fix invented for it.

The work is in the engine (`multiphysics/fem_waves.cpp`, `multiphysics/fem_model.cpp`), the document
(`multiphysicsdoc.cpp`), the tools (`qucscontrol_fem.cpp`, `qucscontrol.cpp`) and the diagrams
(`diagrams/diagram.cpp`, `numberformat.cpp`).

## Fixed

| | What it was | What it is now |
|---|---|---|
| **1** | A 1D plot group's dataset for a sweep of modes declared axes its values did not fill - a mode axis of 2 over 3 widths, 5 values written under a header saying 6 - and which value was which mode moved with the run; the user exported the fields instead | Where the solutions do not fill the axes (or the modes are not the same in number at every frequency) they are written against their own number, with `frequency`, `mode` and each parameter swept beside them: the header says what is there, in the order it was solved. The modes themselves come out in a total order (index, then loss, then polarization), not the eigensolver's |
| **2** | No group index: `emw.ng` did not exist, so it was worked out by hand from a sweep of wavelengths | The mode step reports `emw.ng` = c/vg, from the energy the mode carries over the power along it, with the media's own dispersion in it (their ε and µ at two wavelengths a thousandth apart) |
| **3** | The azimuthal order m of a resonator was a whole number of its own, which no sweep could reach: an order-by-order study was a script that edited the model and solved it again | `m` is an expression of the parameters (`morder`), so a study's sweep runs over the orders - one solution per order, each with its own `emr.m`. What it comes to must still be a whole number |
| **4** | `fem_model new` took a model naming a property no kind has (a misspelt `pos`) in silence, warned about at solve time; `fem_edit` then refused to take it out, since the name that was there was the name it refused | A property no kind has is refused where it is set - by `fem_model new` and `set`, which read a whole model - with the name it was near ("Meant position?"); one already in a model is taken out by `fem_edit`'s null, whether its kind knows the name or not |
| **5** | An export whose file was in a folder that is not there said only that the file could not be written | The folder is made, as the folder of an unsaved model's exports already was |
| **7** | With "decimal" numbers an axis from 2.15 in steps of 0.1 labelled its ticks "2.1", "2.2", "2.4" - the label a tenth of a step from its tick, two ticks named alike; the user started the axis at 2.1 instead | A decimal label is written to as many places as it takes to say where its tick is (within a thousandth of a step), not to the step's places alone |
| **8** | A legend's sample is 20 pixels and Qt measures dashes in pen widths, so a dashed trace of thickness 2 and a long-dashed one of any thickness drew one dash across the whole sample: the legend said "solid line" for every style. With point markers it was worse - the marker's ring of paper broke the middle of the sample, so a solid trace read as broken and a dashed one as solid | The pattern is scaled so two dashes and two gaps fit the sample, its caps flat so they stay gaps, and a row with a marker gets a longer sample - the line is seen on both sides of it |

## Not reproducible

- **6** A standalone `.dpl`'s plots going blank after `edit_diagram` and a save does not happen at
  this commit. Three readings of "standalone .dpl" were built and driven through the user's sequence
  - `add_diagram`, `edit_diagram`, `save_document`, then closed and opened again:
  a `.dpl` written by hand beside its own `name.dat`; a multiphysics 1D plot group's own display
  (`model_pg1.dpl` beside `model_pg1.dat`, made by `fem_plot` and shown again after); and a `.dpl`
  whose `DataSet` names another file. In each, after every step, the graph still had its data, its
  screen points and its curve on the canvas (counted in the trace's own colour), the file kept its
  `<DataSet=...>` line and its trace, and the reopened document drew it again.
  `test_diagram_tools::aStandaloneDisplayKeepsItsPlots` is that sequence, kept as a guard.

  What I did find, for whoever meets it again: a graph goes blank when `Graph::loadDatFile()` reads
  the dataset and does not find the variable - it clears the data first and returns 0 - so a blank
  trace means the file it read had no such variable. The file it reads is the document's own
  `getDataSet()` beside the document, unless the trace's name carries one (`name:variable`). The
  likeliest candidates are therefore a dataset written again under other variable names (a plot
  group whose graphs changed), or a document whose `DataSet` was renamed under it (`Save As` names
  the dataset after the file). Both would also survive `reload_data`, which the user says fixed it,
  so neither is quite their case; the paths that rewrite a display and save it were changed on
  23 October (the B4 and N5 fixes of `showPythonDisplay`), which is the likeliest reason it no
  longer happens.

## 1: an axis that does not span its values

`MultiphysicsDoc::showGraphs()` builds the axes of the dataset from the first solution of the set:
the modes of a frequency, the frequencies, then the sweep's parameters. The values come from
`instances()`, which is every (point, snapshot) pair there is. Nothing checked that the one spans
the other, and a mode analysis is exactly where it does not: `minneff`, `maxloss`, a polarization or
a cutoff drop modes at some points and not at others. The test's guide is 25, 35 and 45 mm wide at
10 GHz with two modes asked for and `minneff` 0.1 - TE20 is above cutoff from 30 mm up, so the
points keep 1, 2 and 2 modes, and five values were written under a `mode` axis of 2 (the first
point's count) over three widths.

Now the axes are measured against the instances, and where they do not fit:

```
<indep solution 5>            1 2 3 4 5
<dep frequency solution>      1e10 x 5
<dep mode solution>           1 1 2 1 2
<dep a solution>              0.025 0.035 0.035 0.045 0.045
<dep emw.neff solution>       the five values
```

`Dataset::notes()` - the reader's own check that a block holds what it declares, from the hunt of
23 October - is empty for this file and was not before. The plot group says why in its messages
("its 5 solutions do not fill the axes a x mode (6 values): each is written against its own
number").

**Padding to a grid was the other way** - a mode axis of the largest count, with NaN where a point
has no such mode, which would keep a curve per mode. Every graph kind in `showGraphs()` walks the
instances and appends a value per instance (globals, derived values, cut lines, cut points), so a
grid with holes means a hole-aware instance in six loops. That is a larger change than the finding
asks for, and the index axis with its columns is what the user asked for ("a deterministic order,
and a header that matches what is actually written").

**The modes' own order** is now a total order: effective index first, then the loss, then the TE
fraction. `std::sort` is no stable sort, so two modes of the same index - a square guide's pair, a
bend's degenerate pair - came out in whatever order the eigensolver left them, and that is the
"order flips between runs" half of the finding. This part has **no test of its own**: a tie needs two
modes whose β are equal to the last bit, and a mesh does not give that - a square guide's pair are
told apart by the mesh's own asymmetry long before the tie-breakers are reached. It is a defensive
total order, written down here rather than tested.

## 2: the group index, and what is in it

`emw.ng` is the energy velocity's: vg = P/U with P the power along z and U the energy per metre,

    U = ¼∫[E*·∂(ωε)/∂ω·E + H*·∂(ωµ)/∂ω·H] dA,   ng = c U / P

which is the group velocity itself where the media take nothing from the mode. ∂(ωε)/∂ω is taken
between two wavelengths a thousandth apart with the point evaluator set to each of them, so a
material's own dispersion counts: an index given as a Sellmeier form or a table of `lambda0` moves
with the wavelength, which is the whole reason the user was sweeping wavelengths by hand. Both
fields are used as they are, before the normalization to 1 W, so the scale falls out of the ratio.

Checked against two closed forms in `test_fem_waves::theGroupIndex`: a hollow metal guide has
ng = 1/neff for every mode (WR-90 at 20 GHz, three modes, to 2e-3), and a guide filled with
n = 1.5 + 0.1 λ1/λ0 has ng = n(n + ω dn/dω)/neff - 2.656564 against 2.656564, where leaving the
media's dispersion out would have given n²/neff, 2.500 (the test refuses that too).

What it does not count: the energy inside perfectly matched layers, which are left out of both
integrals - a mode mostly in them has its ng too low, and the property says so. The two passes over
the mesh are made once per frequency and dropped by each solve (the media follow a coupled study's
temperature and a sweep's parameters), so a run of many modes at one frequency pays for them once.
Each pass goes over the whole mesh at one wavelength on purpose: the media's band-filling and
quantum-well tables are keyed by the photon's energy, and taking the two wavelengths triangle by
triangle would build each table again at every triangle.

## 3: m swept, not a "bend loss per angle" output

The finding offered two ways: make m sweepable, or add a built-in bend-loss-per-angle output. The
first is done - `m` is now an `expr` property, and `ResonatorProblem::setup()` already evaluated a
string as an expression of the parameters, so the engine needed nothing. `emr.m` was already a
global, so a sweep over the orders writes the order beside the frequency and Q in its dataset: a
whispering gallery's or a bend's loss per order is one study with one sweep step, no script.

The second is not done, and I would not do it as "bend loss per angle" either: what a bend study
wants is loss per unit angle (or per turn) of a *bend*, and a body of revolution's eigenfrequency
gives Q and the azimuthal order - the loss per radian is Im ω/Re ω × m, which is `emr.m/(2 emr.Q)`
as a derived value of the user's own. A built-in would have to choose the convention (per radian,
per degree, per turn, dB or Np); that is a decision for the user, not a quiet default.

## 4: where a property is refused

`fem_edit` already refused a property no kind has. The two holes were a whole model given as JSON -
`fem_model new` and `set` keep whatever `Model::fromJson()` read, and only a solve warned about it -
and `fem_edit`'s own null, which was checked after the name was, so the name that was there was the
name it refused. Both are closed. A model **read from a file** is still taken as it is: a file may
come from another version of Qucs-S, and `Model::fromJson()` says what it did not understand. The
near-miss helper `nearNamesOf()` moved from an anonymous namespace in `qucscontrol.cpp` into
`qucs_s::control` so that the fem tools can name it too.

## 7 and 8: the numbers and the samples

A decimal label gets the places the step needs *and* the places its own value needs, to within a
thousandth of a step - `placesFor()` beside `placesOf()`. A number no few places can write (a tick
at 1/3 in steps of 0.1) is written to four, not fifteen. The places asked for by hand are still the
places written.

The legend's sample is 20 pixels (32 for a row with a marker). Qt measures a dash pattern in pen
widths, so `{10, 6}` at thickness 2 is a 20-pixel dash and a 12-pixel gap - one dash, no gap, in the
sample - and `{24, 8}` is longer than the sample at any thickness. The pattern is now scaled to fit
two dashes and two gaps, keeping its proportions so dashes, dots and long dashes still look
different; its caps are flat, because a round cap grows by half the pen's width at each end of every
dash and closed the gaps again; and a row with a marker gets twelve pixels more, so the line shows on
both sides of the marker's ring of paper. The test reads the pixels along each row's sample and asks
for a gap, less ink than a solid line of the same thickness, and - with a marker - a dashed row
plainly more broken than a solid one.

## The breaks, and what was run

`breaks.py` beside this file undoes each fix on purpose; all **16 are caught**, each by the test
named beside it:

| Break | Caught by |
|---|---|
| 1 the axes need not span what is written | `test_multiphysics_doc::aSweepOfUnevenModesSaysWhatIsInIt` |
| 1 nothing says which solution is which mode | the same |
| 2 no group index at all | `test_fem_waves::theGroupIndex` |
| 2 the group index at the one frequency (no material dispersion) | the same |
| 2 the energy counts ε, not ∂(ωε)/∂ω | the same |
| 3 m a whole number of its own again | `test_fem_resonator::theOrderIsSwept` |
| 4 a model as JSON keeps an unknown property | `test_multiphysics_doc::anUnknownPropertyIsRefusedWhereItIsSet` |
| 4 an unknown property cannot be taken out | the same |
| 4 a refusal does not say which name was meant | the same |
| 5 an export does not make its folder | `test_multiphysics_doc::anExportMakesItsFolder` |
| 7 a decimal label gets the step's places alone | `test_number_format::aLabelSaysWhereItsTickIs` |
| 7 a label off the grid gets fifteen places | the same |
| 8 the dash pattern is not fitted to the sample | `test_diagram_legend::theSampleShowsTheLineStyle` |
| 8 a dashed sample's caps are round again | the same |
| 8 a marker row gets no more room | the same |
| 6 (the guard) a display's traces lose their variable | `test_diagram_tools::aStandaloneDisplayKeepsItsPlots` |

Six new test functions; the suites run green after the round (Release, macOS):
`test_number_format` 16, `test_marker_notation` 7, `test_status_bar` 20, `test_value_reading` 29,
`test_diagram_legend` 9, `test_diagram_theme` 9, `test_graph_autocolor` 11, `test_marker_colors` 7,
`test_canvas_drawing` 35, `test_diagram_tools` 42, `test_fem_engine` 27, `test_fem_resonator` 11,
`test_fem_waves` 37, `test_fem_modeoverlap` 8, `test_fem_modulator` 18, `test_multiphysics_doc` 85,
`test_mcp_wishlist` 36, `test_qucs_control` 210, `test_fem_laser` 17. Both lints are green
(`check-tool-arguments.py`, `check-json-nesting.py`).

Built and run in a **copy of the tree** (`BRK_SRC`/`BRK_BUILD` in `breaks.py` say so): another
session was in the middle of `qucs/rf25/` while this round was written, and the checkout itself did
not compile. The copy was the checkout file for file, `qucs/rf25/` held at `aea8c4c`, and nothing of
this round touches that folder.

## Left for later

- **6** is not reproducible here; the guard test is what I can leave behind. If the user meets it
  again, what would settle it is the display's `DataSet` line and the dataset's variable names at
  the moment the plots go blank (`get_schematic` says the traces' names, `get_dataset` the file's).
- **1** pads nothing: a dataset of a sweep of uneven modes is an index with columns, not a grid of
  NaN with a curve per mode. The six loops over the instances in `showGraphs()` are what stands in
  the way.
- **3** has no bend-loss output of its own (see above): `emr.m/(2 emr.Q)` is the loss per radian as
  a derived value, and which convention a built-in should use is the user's call.
- The modes' total order has no test of its own (above).
