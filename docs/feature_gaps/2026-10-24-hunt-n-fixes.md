# The thirteen notes of the evening hunt, worked through

After the A and B rounds (`2026-10-23-hunt-a-fixes.md`, `2026-10-24-hunt-b-fixes.md`), the user: "Work
through the hunt's 13 notes (N1-N13)." The notes are the small end of
`docs/bug_hunts/2026-10-23-the-days-tools.md`: a word spelled wrong, a hint wide of the mark, a number
clamped or dropped where nothing was said of it. Nine are fixed, one was already fixed in the A round,
and three are right as they stand - which is what a note is for, and is written down here rather than
changed.

## Fixed

| | What it was | What it is now |
|---|---|---|
| **N1** | "a rule for **boundarys** is all, numbers, of ..." - the plural made by adding an "s" to `levelName()` | `levelNames()`: domains, boundaries, points, in both messages that speak of more than one |
| **N2** | `fem_evaluate {"reference": "1e-10"}` was looked for as a file: ".../ws/1e-10 could not be read" | Text that is wholly a number is that number; text that is `inf` or `nan` says so rather than being hunted for on disk; any other text is still a file |
| **N3** | `"SI": null` beside `"value": "inf"` - JSON has no number for inf or nan, so a caller reading `SI` saw nothing of the value's kind | No `SI` at all for such a value, and an `SI note` saying which it is: "infinite", "infinite, negative", "not a number" |
| **N6** | A `timeout` given as text was not seen by `waitsFor()`, so a script's `qucs.call('fem_solve', {timeout: '7200'})` was cut at the default's 610 s while the solve ran on | The same number the tool reads is the number waited for |
| **N7** | "Meant **y**?" for `add_analysis {"type": ...}` - a field of one letter is inside almost any word | A name of fewer than three letters counts only against a name of about its own length; a real near miss ("typ" for "type") is still named |
| **N8** | `read_pdf` refused `"all"`, which `read_pptx` accepts; `"1-99999"` was clamped in silence | `"all"` reads the file; pages that are not there, and anything past the 200 a call reads, are listed under "left out"; a word that is no page says what `pages` takes |
| **N9** | A place far out was clamped without a word: `2147483647` became 8388608, `1e300` became 0, and `probe`'s refusal then named "0, 0" | A place is a whole number within the model plane or it is refused, by the number asked for - in `edit_component`, a `set_schematic` part line, `add_diagram`/`edit_diagram` and every tool that takes `[x, y]` through `pointOf()` |
| **N11** | `add_marker {"at": [1000]}` was answered "Say where: 'at', ...", as though `at` had not been given | "'at' is one x value, or peak, min, -3dB, crossing:<y> - not a list" |
| **N13** | A `.dat` whose block says one count and holds another was read down to what is there, the mismatch unmentioned | `Dataset::notes()`, which `get_dataset` reports as "file note": "frequency says 5 values and holds 2. gain holds 3 values over the 2 of frequency." |

## Already done

- **N12** (a Touchstone reference impedance of 0 Ω taken as given) was fixed in the A round, with A9-A11:
  the reader adds "Its reference impedance is 0 Ohm, which no conversion from S parameters can divide
  by." Re-checked against the built app rather than taken on trust.

## Right as they stand

- **N4** `read_pdf` refuses `[0,0,0,0]` and takes `[1,1,1.0001,1.0001]`. The rule is that a region must
  have area, and it is applied evenly: `[0,0,0,100]` - no width - is refused for the same reason, and a
  region with area that holds no text is read and says so (B2's note). Refusing a small region would mean
  choosing a smallest one, which no page size justifies.
- **N5** "Open in Viewer" opens what a picture in the conversation points at without asking. It is the
  reader's own click on a picture Qucs-S drew for them, in their own window; a confirmation there would
  ask the user to approve their own click. The guard that matters - what a *tool call* may open - is
  unchanged.
- **N10** `add_trace` with a variable the dataset has not is added, with a "no data" line saying so. That
  is deliberate: a trace for a variable a later run will produce is how a plot is set up before
  simulating, and the answer says plainly that it shows nothing yet. A diagram or trace *number* that does
  not exist is still refused, because nothing can be made to exist later by simulating.

## N9: where the clamp came from, and where it stays

`Schematic::withinModelLimit()` cuts a point to ±2²³, and `QJsonValue::toInt()` hands back its default
for a double no int can hold - so `1e300` arrived as 0 and `2147483647` as the plane's edge. Both are
right as *defences*: a file or a dialog may hold anything, and the canvas arithmetic must stay inside an
int. What was wrong is using a defence as an answer to a caller who asked for something impossible.

So the clamp stays everywhere a place is **computed** (`add_component {"near": ...}` with a gap, a view
rectangle read from a file, a selection's bounds) and a place **given** is now checked first:

```cpp
QString offThePlane(const QJsonValue& v, const char* key)
{
    if (!v.isDouble()) return {};   // (the tool itself says what it takes)
    const double d = v.toDouble();
    if (std::isfinite(d) && d == std::floor(d) && std::abs(d) <= double(Schematic::ModelLimit)) return {};
    return tr("'%1' %2 is off the schematic: a place is a whole number from %3 to %4")...
```

A fraction is refused by the same line, and for the same reason: `{"x": 10.5}` was read as 0 too, because
`toInt()` takes whole numbers only. `turnAndMove()` asks before it takes the part off its nodes, so a
refusal leaves the schematic exactly as it was.

## N13: what the reader may say and what it may not

A `.dat` block declares its count (`<indep frequency 5>`) or implies it (a `<dep>` over frequency has as
many values as frequency has). The reader already uses that for the room it reserves and then appends what
is actually there, which is the right behaviour - nothing is invented, unlike A9's Touchstone rows - but
it never compared the two. It does now, after the file is read, and the comparison is reported rather than
enforced: a file that disagrees with itself is still read, down to what it holds.

The note is per block and capped at eight, and a block whose count cannot be counted on (no number, a
dependency the file has not) is passed over rather than guessed at.

## Tested

| Test | Suite | What it holds to |
|---|---|---|
| `everyPlaceOffTheSchematicIsRefused` | `test_mcp_wishlist` | N9: four places off the plane in `edit_component` (and the part still where it was), `set_schematic`'s part line, `add_diagram`, `probe` - whose message names 1e+300 and not 0, 0 - and that a place on the plane is still taken while half a unit is not |
| `whatACallAskedForIsAccountedFor` | " | N6 (text, spaced text, another tool's, the default where the text is no number, the bounds), N7 (no hint where there is no near miss, "Meant type?" where there is), N8 ("all", a range past the end, a list with two pages missing, a word that is no page, and nothing said where nothing was left out) |
| `aDatasetSaysWhenItsCountDoesNotMatch` | " | N13: both blocks said, the two points still read, and a file that matches its headers written up not at all |
| `theModelAnswersSayWhatEachNumberIs` | " | N1 (the three levels' plurals), N2 (the number taken, a file still a file, `inf` refused), N3 (no `SI` and the right word for inf and nan, the `SI` still there for a number) |
| `loopDiagrams` | `test_diagram_tools` | N11: `at` as a list refused, by its words, on a trace that has data |

## Broken on purpose

`docs/feature_gaps/2026-10-24-hunt-n-fixes/breaks.py` holds thirteen, one or two per fix: the plural
spelled by adding an "s"; `reference` as text sent back to the disk; the null `SI` written again; the
text timeout unread; the one-letter hint let back in; `"all"` refused again; a missing page passed over;
the place check neutered in the shared helper, then in `turnAndMove` alone, then in `applyDiagram`
alone; `at` as a list read as nothing given; the count comparison skipped; and the comparison made to
fire even where the counts match.

## Driven through the real server

`verify_places_and_pages.json` and `verify_model_and_dataset.json` beside `breaks.py` are the two decks each
fix was watched through `--mcp-server` with, run by the hunt's own `mcpcalls.py`: the places and the pages
against a schematic of two parts, the levels, the reference, the inf and nan values against `coax.qfem`
solved, and the count that lies against a `.dat` written by hand. The tests hold what they showed.

## Not done

- **`set_schematic`'s text form.** A place off the plane is refused in the structured form and in each
  part line of a `<Components>` section, which is where a caller writes one. The `.sch` **file** loader
  still clamps, deliberately: a file written by an older Qucs, or by hand, should open rather than be
  turned away.
- **A wire of no length** given to `set_schematic` is still dropped quietly (`add_wire` refuses one:
  "The two ends are the same place."). The structured form could refuse it as `add_wire` does; the text
  form goes through the file loader, where dropping it is the file format's own tolerance, so the two
  halves of one tool would answer differently. Left for the round that does the loader.
- **N7's hint has no second guess.** Where no name is near enough, nothing is suggested; "type" for
  `add_analysis`, whose field is "analysis", gets the full list of fields and no hint. Suggesting across
  meanings rather than spellings would want a table of synonyms per tool.
