# The wishlist check of 23 October: its seven findings

A live check of N1–N14 in the open window (`~/Desktop/qucs-reviews/2026-10-23-claude-mcp-wishlist-check.md`,
against `472a813` with the 825c4d6 build) found all fourteen working and left seven things to fix. The
user, 23 October: "Work through all seven findings, then archive the file."

All seven are done. One of them was a real gap in what the wishlist round built, not a nit, and two
others grew in the doing: the check's own wording of them was narrower than the thing underneath.

| Finding | Now |
|---|---|
| **1.** `fem_converge` said "converged" where the mesh barely changed: `microstrip.qfem` at `refine: [2, 4]` went 34,373 → 34,924 → 37,450 triangles, and C₁₁ "settled" at 0.0001 % | Every size the mesh is given is divided, not the mesh's own alone - each Size feature's `hmax`, a Layered Region's `hlateral` and `hwall`, with the counts of elements multiplied - and the answer says how much the unknowns really grew. Below 1.5× it reports `"converged": false` with `"not shown"`: the values could not move, so nothing was shown either way |
| **2.** `fem_edit set` took a bare `2e-10`; only the solve refused it | The same check runs where the value is written. It is said, not refused - the value may name a parameter the model has not got yet - under `"check"` in the answer |
| **3.** The unit read "m·s·A" where "C·m" is meant; "or give the number in SI" was odd, since the bare number already is SI | `C·m` is in the display table, and the message ends "or give a value within that range" |
| **4.** A reference row matching no value (`Z 5`) was only counted | Each one is named, through the `"ignored"` field N2 built for exactly this |
| **5.** `undo` with `step: "1"` became `steps` and was then refused, because `"1"` is text | A number written as text is **read as that number** where the field takes a number, and the answer says so |
| **6.** The selection's text ended its lines `\r\n`, as `read_pdf` did too | Both give `\n` |
| **7.** `read_pdf`'s `region` cropped only the picture | It keeps the text to the lines whose box meets it, so the lines a selection gives are read by one call |

## What each change is

**1, a mesh that really gets finer** (`qucscontrol_fem.cpp`). The first cut divided the mesh node's own
largest element and nothing else. A model whose mesh is set by local features - a Size feature on a
domain, a Distribution's count along a boundary, a Layered Region's lateral size - then kept almost the
mesh it had, and the values of course did not move; the answer called that "converged". `refineMesh()`
now walks the mesh's features and divides every length they give (`hmax`, `hlateral`, `hwall`) and
multiplies every count, with the mesh's own, so the whole mesh is refined by the factor. On
`coax_rlgc.qfem`, which has Size features, that is 4 sizes divided and 81,324 → 234,636 unknowns at a
factor of 2, where dividing the global size alone had moved far less.

Two more things came out of reading that code again:

- Turning `custom` on to set `hmax` applied whatever `hmin`, `growth`, `curvature` and `narrow` the node
  still held from an earlier experiment - those fields are hidden but kept when custom is off. They are
  now cleared when the model was not custom, so the preset's own values stand; and a *custom* `hmin` is
  divided with the rest, since it would otherwise clamp the refinement it is supposed to allow.
- The verdict is no longer a word about values alone. The answer carries `"the finest mesh"` ("2.89 times
  the unknowns of the one before, 4 sizes divided") and, for each refined mesh, `"sizes set"` - every
  size by name, `{"hmax": "0.5", "sz1.hmax": "0.025", "dis1.count": 24}` - so what was refined can be
  read rather than guessed. Where that ratio is below 1.5 `"converged"` is **false** with `"not shown"`
  saying why: the question was not put, so neither answer was earned. That holds whatever the cause - a
  geometry's own small features through the preset's curvature and narrow-region settings, a count
  already fine - and it is the part of this finding that cannot go stale.
- Writing the tests for this turned up a third way the verdict could lie: where every value failed to
  evaluate, the table of moves was empty, nothing had moved, and the answer said "converged" of an
  experiment it had not performed. It now reports `false` and says no value could be compared.

**2, checked where it is written** (`qucscontrol_fem.cpp`, `multiphysics/fem_expr.{h,cpp}`,
`multiphysics/fem_model.cpp`). `outsideItsScale()` moves out of its anonymous namespace into
`fem_expr.h`, so the panel and the tools can ask it the same question the solver asks. `fem_edit`'s
`apply()` now compiles each expression value it writes, in the model's own parameter scope, and where it
is a constant with no unit of its own and outside its property's scale it adds a line to `"check"`. It
does not refuse: the expression may name a parameter that is about to be defined, and the solve refuses
it in any case.

That needed the property to know its unit, and `dlinewidth` declared none - which is why the solver's
`Dim::of(1, 0, 1, 1)` at the call site was the only place that knew. It now declares `C*m`, the unit a
bare number there is read in. (`PropertyDef::unit` is a display hint - the dialog's placeholder, a column,
a line of `fem_describe` - so naming it changes nothing else, and the dialog reads better for it.)

**3, how the message reads** (`multiphysics/fem_expr.cpp`). `C·m` joins the display table, so an energy
per field is named rather than spelled out of the base units it happens to be made of. And the last
clause said "or give the number in SI" to someone who had just given a number in SI; it now says "or give
a value within that range".

**4, the rows left over** (`qucscontrol_fem.cpp`). `valuesAgainst()` records which reference rows were
met, and `fem_evaluate` names the rest through `ignoredForCall()` - the channel N2 built - with the names
the values do answer to. Only where something matched: where nothing did, the existing "none matched"
already lists them all, and saying it twice would be worse.

**5, a number written as text** (`qucscontrol.cpp`). The check's reading was that such a value should be
*accepted*; accepting it would have been worse than refusing it. A tool reads these fields with
`toInt()`, which gives 0 for a JSON string, so `undo {"steps": "2"}` let through as it stands would have
undone nothing and said it undid 2. `asNumbers()` therefore **converts**: where the field's declared type
is `integer` or `number` and the whole of the string is a number (a whole one for an integer field), the
argument is replaced by that number and the answer says so. Anything else - `"1.5"` for an integer,
`"a lot"`, `"500[ms]"` for a field that takes text - is left exactly as it was, and refused as before.
It runs for every tool, not only for a singular that was just renamed, since the same slip is one
keystroke away in either.

That is a deliberate change to something an older test held: `test_qucs_control`'s
`decibelsTheOperatingPointAndMarkers` listed `"5"` for a marker's `precision` among the values that must
be refused, beside 13, -1 and 2.7. The three that are wrong are still refused - and so are `"13"` (read
as 13, then out of range) and `"2.7"` (not whole, so not read as a number at all) - while `"5"` is now
read as 5 with a word about it. The test says that instead.

**6, one line ending** (`qucscontrol.cpp`, `qucscontrol_design.cpp`). The PDF module gives `\r\n`, as the
page has it. Both places that hand that text on - the selection in `get_state` and `get_ui`, and
`read_pdf` - normalise it.

**7, a region that narrows the text** (`qucscontrol_design.cpp`). `read_pdf` reads a page with
`getAllText()`, whose `bounds()` gives one box per line. With a `region`, the lines whose box meets it
are kept and the rest left out, so `read_pdf` with the boxes `get_state` gives for a selection reads
those very lines. Where the module gives a number of boxes that does not answer to the number of lines,
the page is read whole and `"of the region"` says so rather than guessing.

## Tests

`test_mcp_wishlist` gains two functions and four of its own are extended; it has 17 now, and
`test_qucs_control` carries the one expectation finding 5 changed.

| test | what it holds to |
|---|---|
| `aWeakRefinementShowsNothingAndAnEditIsChecked` | a 5 % refinement gives `"converged": false` with `"not shown"`; `coax_rlgc.qfem` (local Size features) more than doubles its unknowns at a factor of 2 and says how many sizes were divided; a model with a Size feature, a Distribution and stale hidden fields has its count doubled, its two sizes halved, no `hmin` among them and 2.6 times the unknowns; a value that evaluates nowhere gives `"converged": false`; a bare `dlinewidth` is said under `"check"` and written all the same, while the same value with its unit, and a plausible one, say nothing |
| `aRegionKeepsThePdfsLinesAndOneLineEnding` | a PDF of four lines at known heights: no `\r` anywhere; the top region gives lines 1-2 and the lower one 3-4; `"of the region"` says what was done; a region off the page is refused |
| `aBareNumberOutsideItsUnitIsRefused` (extended) | the message names `C·m` and not `m·s·A`, and says "within that range" and not "the number in SI" |
| `aSingularIsReadAsTheListItNames` (extended) | `undo {"step": "2"}` undoes both components and says both what it renamed and what it read as a number; `"1.5"`, `"a lot"`, a text field and a path of `"1"` are left alone |
| `aMeshConvergesAndIsMeasured` (extended) | the finest mesh has more than twice the unknowns of the one before, the answer says so, and `"not shown"` is absent; a reference row no value answers to is named under `"ignored"` |

## Broken on purpose

`breaks.py` gains 20 breaks (C1-C7), one to six per finding, and holds 74 in all. Each is applied, the
tests it names are built and run - each must fail - and the source is put back.

Two of them went uncaught at first, and both for the same reason: the fixtures did not exercise the
line. `coax.qfem` and `coax_rlgc.qfem` have no Distribution and no stale hidden fields, so breaking the
counts or the clearing of those fields changed nothing anybody could see. The fix was to make what the
refinement sets visible (`"sizes set"`) and to write a fixture that has both - a small rectangle with a
Size feature, a Distribution of 12 on a boundary, and a mesh that is not custom but still holds
`hmin: "4"` and `growth: "1.01"` from an imagined earlier try. With the stale 4 mm applied, that mesh
grows 1.71 times instead of 2.62 and `"sizes set"` carries an `hmin` it should not have.

(One chase in the middle was my own doing: a run of the test against a library the break had left behind,
which showed an `hmin` that the source no longer wrote. Rebuild after a break run before believing
anything it says.)

The 29 suites of the multiphysics side pass (they solve every shipped example, which is what finding 1's
refinement and finding 2's check could have broken), and 25 of the tools, the diagrams and the documents,
with the two CI lints.

## Not done

- **A plausible range per property** (still). Finding 2 checks against the unit's scale, which is what the
  property declares; a density's or a dose's SI value spans everything a device holds, so a bare number
  for one still cannot be called wrong.
- **A number written as text inside an object.** Finding 5 converts the call's own arguments. A numeric
  string nested in an object argument (`eye: {"bit_period": "1e-9"}`) is still refused by its type.
- **`region` over two pages.** It is applied to each page asked for, separately; a selection that runs
  from the foot of one page to the head of the next is two regions, and the check did not try one either.
