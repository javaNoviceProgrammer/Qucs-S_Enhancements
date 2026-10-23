# The MCP wishlist of 22 October: its fourteen new items

The user, 23 October, sent `~/Desktop/qucs-reviews/2026-10-22-claude-mcp-wishlist.md` and chose **all the
new items** of it: N1 to N14, the list written from that session's own work. (The same file's older
sections - the general wishlist of 7 October and the eye-analysis wishlist of 6 October - were not part
of this round; their status table in the review file stands as it is.)

Every item is done. Two of them turned out to be half there already, and the report says so where it is
so: N4's trace expressions, and N6, whose mismatch was a drawing bug under it. The review's first top
pick, the command-guard bypasses (`alias … shell` and backquote substitution), is not one of the new
items and was fixed the same day in `63c2d2c`, with `erc::runsByAnotherName()`.

| Asked for | Now |
|---|---|
| **N1.** Refuse or warn on a value implausible for its unit: a bare `dlinewidth="2e-10"` (read as C·m) once used all the RAM | `evaluateConstant()` refuses a number with no unit of its own whose SI value is far outside what that unit is ever worth, and says what it read, what such a quantity is, and how to write the unit in brackets. The bounds are a table of thirteen units, generous by several orders, and only for units whose scale is not in doubt |
| **N2.** Say when an input was ignored or renamed - `import_data` turns `-` into `_`, `'of'` in a mesh selection is dropped | Every answer can carry `"renamed"` and `"ignored"`, merged into its own JSON by `callTool`. `import_data` fills `"renamed"` with the file's column names against the names they are read under; a selection that holds two rules says which was read and which were left out |
| **N3.** Tell how a call will be treated before it is made | `get_state` has a `"guards"` object: which guards are on, what each watches, and whether this conversation is asked at all. `describe_tool` takes `"arguments"` and answers `"this call"`: let through, asked about each time (with the very words the user would see), or refused within the call under way |
| **N4.** Scale a trace or an axis without copying the data; a per-axis notation | A trace may be an expression of the dataset's variables already (`li:semi.I_anode*1e3` plots a current in mA, and a re-solve follows it) - it was in `add_trace`'s description and not in `add_diagram`'s, which it now is. New: each axis takes its own `notation` and `decimals`, kept in the file, with `null` giving the diagram's back |
| **N5.** Keep the styling when a FEM plot is shown again | A 1D plot group shown again keeps the diagram that is there - its ranges, colours, markers and paintings, as before - and no longer writes over an axis label of the user's own words; only a label that is the same words in another unit is followed. `fem_plot` says what it kept |
| **N6.** Make `screenshot area: screen` match the window | It does, and did: the mismatch was the drawing bug under it. A shape filled with a colour of its own is now paper itself, so a black text on a white box is drawn black on the dark canvas - as prints and exports always had it - and the picture of the canvas is that very drawing |
| **N7.** Check mesh convergence in one call | `fem_converge`: the study solved at the mesh as it is and at finer ones, each mesh's elements, unknowns and seconds, every value watched at it, and between the last two meshes each value's change in absolute terms and per cent with whether it converged. The mesh node is put back |
| **N8.** Compare against reference numbers in one step | `fem_evaluate` takes `reference`: a number, `{name: number}`, or a table's file of names and numbers. Each value then carries its `reference`, `difference` and `difference %`, and the answer the largest difference |
| **N9.** `connect` to `"ground"` should reuse a ground already there | A ground symbol that was placed and left connected to nothing, in a straight line from the pin and within 200 units, is wired to instead of a second one being placed |
| **N10.** A script's error should say what really happened, and its timer should not count the user's time | A stopped script says whether its own code was running, which call was under way, or which call it had just made. The time a call spends waiting for the user to answer a card is counted apart and taken off the script's clock; the answer carries it as `"waited for the user"` |
| **N11.** `delete` with `name` (singular) should be accepted | A singular given where the tool takes the plural is read as that field - wrapped in a list where the field is one - and the answer says so under `"renamed"`. It covers every tool: `delete` with `name`, `read_pdf` with `page`, `undo` with `step`, `fem_plot` with `color` |
| **N12.** Give the PDF viewer's selection as data | `get_state` (per document) and `get_ui area: document` carry a `"selection"`: a PDF's selected text with its page, its lines' boxes in page points and the page's size; a text tab's selected text with its first and last line |
| **N13.** Let a screenshot be smaller | Every picture's longer side is at most 1500 pixels now (it was the width at 1800), and `max_width`, `scale`, `gray` and `format` (png or jpeg) make it smaller still. The size the answer gives is the size sent |
| **N14.** Say what a `screen` shot shows | For a PDF: the pages in view, each one's part of it in points, the page's size and the zoom, so that `read_pdf` can follow the same lines. For a presentation: the slides and their parts. For a text tab: the lines in view, of how many |

## What each change is

**N1, a bare number in SI** (`multiphysics/fem_expr.cpp`). `evaluateConstant()` reads a property's value
as a constant of an expected unit, and a number with no unit of its own is taken as that unit's SI
value. That is the trap the review hit: `dlinewidth` is an energy per field, C·m, so a bare `2e-10` is
1.25 GeV per V/m, and the gain of a well with that broadening took every byte the machine had. A new
`outsideItsScale()` holds a table of thirteen units with what a quantity of each is at the very outside
- an energy 1e-26 to 1e-12 J, an energy per field (a dipole moment) 1e-35 to 1e-22 C·m, a voltage,
a field, a current, a power, a temperature, a time, a mass, a capacitance, a resistance, a conductivity,
a thermal conductivity - and a value beyond those is refused, with what it was read as, what such a
quantity is, and how to write the unit (`1[meV]/(1e7[V/m])`). The bounds are several orders past
anything a device has, so only a number no one means is refused. Zero always passes, as does anything
whose unit was written. Units whose SI value spans everything a device holds are **not** in the table and
cannot be judged this way: a density (a bare `1e16` for 1/m³ is the intrinsic carriers of silicon, and a
plausible number for someone thinking in cm⁻³) and an implant dose are left alone. A property's own
plausible range would catch those, and is not here.

**N2, what was renamed or left out** (`qucscontrol.cpp`, `qucscontrol.h`, `dataimport.*`,
`qucscontrol_results.cpp`, `multiphysics/fem_geometry.cpp`). `renamedForCall(from, to)` and
`ignoredForCall(what, why)` collect what a call read under another name or did not use; `callTool`'s
`withNotes` merges them into the answer - into its own JSON as `"renamed"` and `"ignored"` where the
answer is JSON (keeping its indentation), else as a line after it. `di::Data` keeps a `renamed` map,
filled in `variablesOf()` where a column's name is made safe or numbered, and `import_data` passes it
on, so `{"renamed":{"i-in":"i_in","v-out":"v_out"}}` comes back with the import. In a selection,
`resolve()` now records which rule it read and names the others of the object that it did not: `{"all":
true, "of": ["x"]}` picked everything and said nothing, and now says `all was read, and of of the same
selection left out`. `side` counts as left out where the rule read is not `of`; `except`, which is read
with any of them, and a false `all`, `exterior` or `interior` do not.

**N3, how a call would be treated** (`qucscontrol_resources.cpp`, `qucscontrol.cpp`). `guardsState()`
answers which guards are on (the commands guard, the files-outside-the-workspace guard), what each
watches in words, and whether this conversation is asked at all - the thing the review had to read the
settings file to learn. `callGuarded(tool, arguments)` answers how one call would be treated, from
`askedEachTime()` and `irreversible()` and the read-only list, with the very words the user's card would
carry; inside a call that runs alone it adds what would be refused outright (`unaskedWithin`).
`get_state` carries the first, `describe_tool` both - its new `arguments` field makes nothing happen.

**N4, a trace and an axis scaled** (`diagrams/diagram.h`, `diagrams/diagram.cpp`,
`qucscontrol_results.cpp`, `qucscontrol.cpp`). The first half was there: `Graph::loadExpression` has long
evaluated an expression of the dataset's variables at each sample, dotted names included, so
`add_trace` with `li:semi.I_anode*1e3` plots the current in mA and follows a re-solve. It was said in
`add_trace`'s description only, which is where the review did not look; `add_diagram`'s `traces` now says
it too, with that very example. What was missing is a notation of its own per axis: `Axis` gains
`notation` (-1: the diagram's) and `decimals` (-2: the diagram's), `Diagram::notationOf()` and the new
`decimalsOf()` read them, and the diagram's line keeps them in a quoted field of its own after the
colours (`"xn=4 xd=2 yn=1"`), which older versions do not read and newer ones fill in. `x_axis`,
`y_axis` and `y2_axis` of `add_diagram` and `edit_diagram` take `notation` and `decimals`, `null` giving
the diagram's back. A bathtub diagram's own `notationOf` puts an axis' choice first.

**N5, a plot group's styling** (`qucs_python.cpp`, `qucs.h`, `multiphysicsdoc.*`,
`qucscontrol_fem.cpp`). `showPythonDisplay` already kept a diagram whose traces are the ones asked for -
so ranges, colours, markers and paintings were never lost - but it wrote the plot group's own axis
labels over whatever was there. Now a label is written over only when it is the same label: its words
with every parenthesised unit removed, compared. "I (A)" gives way to "I (mA)", and "Anode current"
stays. What stayed is reported through a new `kept` out-parameter, said by the model tab and carried by
`fem_plot` as `"kept as the user set it"`.

**N6, the canvas and its picture** (`paintings/shapepainting.cpp`). The review's two bugs were one:
`screen` and `window` drew the same pixels, and both drew what the dark-canvas recolouring made of a
black text - pale grey on a white box, because `ink::on()` judged the text against the canvas while the
box's own fill sat between them. `ShapePainting::paint` now sets the fill it is about to draw as the
paper (`ink::Paper`) for the border and for everything the shape holds, where the fill is opaque enough
(alpha 192 and up) for the canvas not to show through. The fill itself is still judged against the
canvas, since that is what it is drawn on. So a white note box has black words and a black border on the
dark canvas, as on paper, and a see-through box behaves as before. The text box, the table and every
other filled shape go through that one place.

**N7, mesh convergence** (`qucscontrol_fem.cpp`). `fem_converge` reads the mesh's largest element with
`meshSizes()` - whatever says it, the preset or a customized `hmax` - then solves the study once per
mesh: the mesh as it is, then that element size divided by each factor of `refine` (`[2]` by default,
five meshes at most). Each solve goes through `fem_solve`, so it waits the way that tool waits, and each
value watched goes through `fem_evaluate`, so it takes whatever that tool takes - a global, a Derived
Values node's value, an expression. Between the last two meshes it reports each value's change, in
absolute terms and per cent, against `tolerance` (0.01). The mesh node is put back as it was in a
`finally`-shaped step that runs whatever happened, and the model is left as unsaved as it was; the
solution kept is the finest mesh's, which the answer says, since the model then counts as changed since
it was solved.

**N8, reference numbers** (`qucscontrol_fem.cpp`). `fem_evaluate` takes `reference`: a bare number (the
one value's, whatever it is named), an object of name to number, or a file - its rows a name and a
number, apart by a comma, a semicolon, a tab or spaces, with comment lines (`#`, `!`, `%`, `//`) and a
header left out, read beside the model. Each value carries `reference`, `difference` and `difference %`;
the answer says how many references there were, how many values were measured, and the largest
difference with its name. Where no name matched it says so and lists the references, rather than
answering as if nothing was asked. Against a swept x it matches by the name the sweep's label gives each
value; a column of x values against a column of references is not read that way.

**N9, a ground placed and left** (`qucscontrol.cpp`). `looseGroundNear()` looks for a GND component that
is in the circuit, whose own pin is connected to nothing, in a straight line from the pin (the same x or
the same y) and within 200 units - the nearest such. `groundPin()` wires the pin to it with
`placeGround`'s own idiom (a snapshot, the nets before, `netChangesBeyond` as the check passed to
`wireUp`) and says where the ground was; if the wiring would change another net, the snapshot is
restored and a ground of its own is placed as before.

**N10, a script's error and the user's time** (`qucscontrol_script.cpp`, `qucscontrol_resources.cpp`,
`qucscontrol.h`). `askUser()` - the card the panel shows for an elicitation - publishes when its wait
began (`a_askedUserSince`, on a monotonic clock both threads can read) and adds the wait to
`a_askedUserMs` when it ends; `askedUserMs()` counts the wait under way as far as it has gone, so the
script's watchdog - another thread - sees the user's time while it passes and not only once it is over.
That mattered: the first cut added the time only afterwards, so a card answered after 1.6 s still
stopped a script whose timeout was 1 s, and the sharpened test caught it. The watchdog waits in slices
of 200 ms and stops the script when its *own* time, the wall clock less the user's, passes `timeout`;
the answer's `seconds` is that own time, with `waited for the user` beside it where a card was answered.
The message of a stopped script says what it was doing: which call was under way, else which call it had
just made (the engine stops between calls more often than inside one), else that its own code was
running and it had called no tool.

**N11, a singular for a plural** (`qucscontrol.cpp`). `asPlural()` looks at every argument the tool does
not take and, where its plural (`+s` or `+es`) is a field the tool does take, reads the value as that
field: wrapped in an array where the field is a list, as it is otherwise - `read_pdf`'s `pages` takes
`[3, 4]`, `"2-5"` or `7`, and `undo`'s `steps` is a number. It fires before `unknownArguments`, records
the rename for N2's `"renamed"`, and calls the tool again with the arguments put right; a plural already
given is not written over. The first cut read only lists, which is what the review asked for; the break
run showed that restriction was both untestable and too narrow, since the same slip reaches `undo`'s
`steps`, `console`'s `lines`, `send_input`'s `keys`, `fem_plot`'s `colors` and `export_image`'s
`colours`, and reading the singular as the field is right in each. No field was found whose singular
means something else.

**N12, the selection as data** (`pdfdoc.h`, `qucscontrol.cpp`, `qucscontrol_ui.cpp`). `PageView` gains
`selectionPage()` and `selectionBounds()` - it already kept both, by character range, so a selection
that runs past what is in view is whole. `selectionOf(doc)` answers a PDF's selection as its text, its
page, its lines' boxes in page points (what `read_pdf`'s `region` takes) and the page's size, and a text
tab's as its text with its first and last line and its length. `get_state` carries it per document and
`get_ui area: document` for the one in front - both places the review looked. A presentation has no text
selection in Qucs-S at all, so there is nothing to give for one.

**N13, a smaller picture** (`qucscontrol.cpp`, `qucscontrol_p.h`). `PictureAsked` holds what a reader
asked for - `max_width` (the longer side, 1500 by default where it was 1800 on the width alone), `scale`,
`gray`, `format` - read once at the top of `screenshot` and passed to `fitted()` and `pngContent()`. A
JPEG is written at quality 80. Every picture `screenshot` sends goes through them, and the size the
answer names is the size sent (the PDF-page branch said the size before the cut, and its dpi now follows
the cut too).

**N14, what a picture shows** (`qucscontrol.cpp`). `shownInView(doc)` walks out from the page or slide in
sight while the pages are in view and says, for each, its number and its part of it in points with the
page's own size (or that it is in view whole), and the zoom - for a PDF, for a presentation, and for a
text tab its first and last line of how many. It is said after the `screen` or `paper` picture of a
document's window.

## Tests

`tests/test_mcp_wishlist.cpp` is new, `qucs_add_test(test_mcp_wishlist)` beside the other wishlist's; one
function of `test_fem_engine` is extended.

| test | what it holds to |
|---|---|
| `aBareNumberOutsideItsUnitIsRefused` | a bare `2e-10` for C·m and `6.6e-3` for an energy refused, saying it was read in SI and how to write the unit; `1[meV]/(1e7[V/m])` and `6.6[meV]` read; 300 K, 0, 1e-12 s, ±3.3 V, 1e-6 and 5 m, 1e7 V/m, 9.1e-31 kg taken; `1e16` for 1/m³ taken (no scale to judge by) |
| `aSingularIsReadAsTheListItNames` | `delete {"name": "R2"}` deletes R2 and says `name was read as names`; `read_pdf`'s `page` and `fem_plot`'s `color` read unwrapped, `delete`'s `name` wrapped; `undo {"step": 2}` undoes both components (a list of one would be refused by its type); a plural already given and an argument of the tool's own left alone; a word that is no singular refused as before |
| `aRenamedInputIsSaid` | `import_data` of `time,v-out,i-in` answers `"renamed"` with both, and the columns as used; a file of plain names says nothing |
| `theGuardsAreToldBeforeACallIsMade` | `get_state`'s `"guards"`; `describe_tool` with `arguments` for a file outside the workspace is "asked about each time" and names the file; an ordinary edit and a tool that only looks are let through; no `arguments`, no `"this call"` |
| `groundReusesOneLeftUnwired` | one GND left at 100,160 is wired to, one ground in the schematic, both pins on `gnd`; then each thing that rules a ground out, one schematic each - in line but 470 units away, 120 units away but in neither line, in line and near but with a wire of the user's on it - gives two grounds |
| `anAxisWritesItsNumbersItsOwnWay` | x decimal to two places while y is in SI prefixes, in the answer and in `numberText`; saved, closed and read again; `null` gives the diagram's back |
| `aPlotGroupKeepsWhatTheUserSet` | "I (A)" gives way to "I (mA)"; "Anode current" stays and is reported; an empty label is filled in; the diagram is the one that was there, with its manual range and its trace's colour |
| `aFilledBoxIsItsOwnPaper` | the box painted by itself on the dark paper: inside its white fill more dark pixels than pale ones, and as many as on white paper (within a quarter); the `screen` picture is the canvas's own drawing, pixel for pixel |
| `aPictureIsAsSmallAsAsked` | a window picture no longer than 1500 px; `max_width` 400 with `gray` and `format` jpeg honoured, the answer saying jpeg and the bytes beginning FF D8 FF, and a PNG's signature where none was asked; `scale` 0.25 smaller than the default; a width of 10, a scale of 9 and a format of "tiff" refused |
| `aScreenPictureSaysWhatItShows` | a 400-line netlist's picture says which lines are in view, of how many |
| `theSelectionIsGivenAsData` | nothing selected, no field; two lines selected give the text, the first and last line, in `get_state` and in `get_ui area: document` |
| `aMeshConvergesAndIsMeasured` | `fem_evaluate` with `reference` 1e-10 gives -2.96 %; a CSV with a header and a comment matches two values; a file that is not there and a reference that is no number refused; a reference no value answers to says "none matched". `fem_converge` over `[2, 4]`: three meshes, each with more triangles and a smaller element, every value at each, the coax's capacitance converged to under 0.01 %, the mesh node and the model's unsaved state as they were; an empty `values`, a refinement of 0.5 and a tolerance of 0 refused |
| `aStoppedScriptSaysWhatItWasDoing` | a script of its own code says it had called no tool; one stopped around calls names `add_component` and its call number; a card answered after 1.6 s inside a script whose timeout is 1 s does not stop it, and `seconds` leaves the user's time out |
| `selectionRules` (extended) | `{"all": true, "of": [...]}` picks everything and says `all was read` with `of` left out; `numbers` before `of`; `side` beside another rule said; `of` with a side, `except`, and a false `all` not complained of |

`test_mcp_wishlist` (15 functions) passes, and so do the 29 suites of the multiphysics side
(`test_fem_*`, `test_multiphysics_doc`, `test_multiphysics_view`, `test_process_console`) - which matters
for N1, since they solve every shipped example and a bare number refused wrongly would have stopped one
- and the suites of the tools, the diagrams and the documents (`test_qucs_control`,
`test_wishlist_tools`, `test_mcp_server`, `test_claude_code`, `test_claude_fuzz`, `test_diagram_tools`,
`test_canvas_drawing` and its ratio-2 twin, `test_new_paintings`, `test_dataset`, `test_data_import`,
`test_data_export`, `test_pdf_viewer`, `test_image_doc`, `test_markdown_doc`, `test_pptx_doc`,
`test_graphics_export`, `test_number_format`, `test_prbs_eye`, `test_scale_and_memory`), with the two CI
lints (`check-json-nesting.py`, `check-tool-arguments.py`).

## Broken on purpose

`docs/feature_gaps/2026-10-23-mcp-wishlist/breaks.py` holds **54 breaks**, one to six per item (N1 3,
N2 6, N3 3, N4 5, N5 3, N6 1, N7 4, N8 4, N9 4, N10 5, N11 5, N12 3, N13 6, N14 2): each is applied to
the source, the tests it names are built and run - each must fail - and the source is put back.

The first pass over 48 of them caught 43. The five that got through were the tests' fault, not the
breaks':

- **N6** counted dark pixels over the whole canvas, where a few lie outside the box anyway. It now
  paints the box by itself on dark paper and on white and counts the pixels inside its own fill, and it
  holds the `screen` picture against the canvas's drawing pixel for pixel rather than counting again.
- **N9** had one ground at 400,600, which three different rules each keep out, so breaking any one of
  them changed nothing. Each rule now has a schematic of its own: in line but out of reach, within reach
  but in neither line, in line and near but with a wire of the user's on it.
- **N10** gave the card 900 ms against a 1 s timeout, so counting the user's time or not made no
  difference. At 1.6 s it does - and then it failed for real: the user's time was added to the counter
  only after the wait, so the watchdog still fired at 1 s. That is the N10 fix's own bug, found here and
  fixed (the counter now carries the wait under way).
- **N11**'s "a field of one type taken as a list" could not be caught at all, because the only argument
  the test gave was one the tool takes itself. Looking for a case that would catch it showed the rule was
  too narrow (see N11 above); it now reads any plural, and `undo {"step": 2}` tells a list of one from a
  number.
- **N13** read the answer's `mimeType`, which is set from what was asked and not from what was written.
  It now reads the bytes: FF D8 FF for the JPEG, and the PNG signature where none was asked for.

With those sharpened, and the 6 breaks the new rules brought with them, **all 54 are caught**.

## Not done

- **A plausible range per property.** N1 judges by the unit, not by what the property is: a density's or
  an implant dose's SI value spans everything a device holds, so a bare number for one cannot be called
  wrong. Each property would have to carry its own range for that.
- **A reference column against a swept x (N8).** References are matched by name. A table of x values
  with a column of references, read against a sweep's own x, is not read that way.
- **A presentation's selection (N12).** Qucs-S's slide view has no text selection, so there is nothing to
  report for one. The field is there for a PDF and for a text tab.
- **A trace `scale` of its own (N4).** An expression does it (`meas:i_in*1000`), and a second way to say
  the same thing would be a second thing to keep in the file and in the dialog.
