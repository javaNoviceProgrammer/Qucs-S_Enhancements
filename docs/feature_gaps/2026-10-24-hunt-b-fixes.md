# The B-class findings of the evening hunt, fixed

After the A-class round (`2026-10-23-hunt-a-fixes.md`), the user: "Fix the B-class findings (B1-B10) the
same way." All ten are done. They have a shape in common - each is a tool that **did** something with an
argument or a file and said nothing, or said the wrong thing about it - so the fixes are mostly a matter
of refusing what cannot be, and saying what was changed.

| | What it was | What it is now |
|---|---|---|
| **B1** | A `reference` named for a value that does not exist was measured against anyway, and counted as matched | The "one number for one value" rule applies only to a number given **without** a name; a name that matches nothing is reported as matching nothing |
| **B2** | A region that meets no text made `read_pdf` call the PDF scanned | It says the region holds none of the page's text, and that the page may well have some |
| **B3** | A geometry size of `1e308`, `nan` or `notaparam` was taken without a word | A size beyond any geometry is refused; one that does not read as an expression is said at the edit |
| **B4** | A plot group shown again marked its display changed, so `showGraphs` rewrote the `.dpl` | The three lines that mark it changed run only when a label was actually written |
| **B5** | A count of nought was clamped in silence in three tools | `fem_converge`'s `timeout`, `tune`'s `max_runs` and `find_shapes`'s `limit` each refuse a count outside their range, by name |
| **B6** | `line_calc` answered `nan` for a height, width or permittivity of nought or less | Each is refused by name, and a result that comes out as no number is refused as the synthesizers' are |
| **B7** | `receiver_budget` took a temperature below absolute zero, a negative bandwidth and a noise figure below 0 dB | Each is refused by name, and every level is bounded at both ends |
| **B8** | `trigger_action {"action": ""}` fired the first menu item (File > New) | Nothing names no action |
| **B9** | A CSV whose rows are not all one width lost its header names **and every row ahead of the widest one** | The header is found and the rows are kept; the mismatch is said |
| **B10** | Three columns of one name reported only the last rename | They are said outright, in order |

## B9: worse than the hunt found

The report said a ragged CSV "loses its header names in silence and gains a column". Reading `tableOf()`
showed it also **drops data rows**. The table's width is the commonest row width (ties to the wider), and
`first` - where the numbers begin - was the first row of exactly that width:

```
a,b,c      <- the header
1,2,3      <- dropped: it comes before the widest row
4,5        <- dropped
6,7,8,9    <- 'first': the widest row, so the header is looked for just above *this*
```

So `a,b,c` was never seen (the line above `first` is numbers, which ends the search) and two of the three
data rows went with it: one row came back, under `A, B, C, D`. `first` is now the first row **within one
value** of the width, which finds the header and keeps every row - and still steps over a count written
on a line of its own ("3" above three columns), which is more than one value off and was the reason for
the strict rule in the first place:

```cpp
const qsizetype least = std::max<qsizetype>(std::min<qsizetype>(2, width), width - 1);
```

`ragged.csv` now reads `a, b, c, D` with all three rows (the short one filled with nothing), and says:
"Its rows are not all the same width: 1 of 4 values, and 1 row(s) of 2, 1 row(s) of 3. The table is 4
columns wide - a shorter row is filled with nothing, a longer one is cut - so check what wrote the file."

## B10: a map cannot hold them

`data->renamed` is keyed by the name **in the file**, so three columns called `a` all mapped to the same
key and only the last survived: `{"a": "a_3"}`, with nothing said of `a_2`. The names each column became
are now collected in order, and a name used more than once is said outright instead of being squeezed
into the map:

```
3 columns are called "a" in the file: they are read as a, a_2, a_3, in that order.
```

The map is left for what it is good at - one column read under another name, which `get_dataset` and
`add_trace` use to find it - and is not made to say something half true.

## B6, B7: the two tools that trusted their programs

`line_calc` and `receiver_budget` pass their numbers to `qucstrans` and `rxcalc` and hand back whatever
comes of it. Every other synthesizer checks its inputs and refuses a design whose values come out as no
number; these two did neither. They do now, with two small helpers (`mustBeAbove`, `mustBeWithin`) that
judge only values given **as numbers** - one written with a unit ("1[mm]") is still the program's to
read - and a `noNumberIn()` net over the answer:

```
line_calc {"h": 0}        -> "No line: 'h' is the substrate's height, above 0 - 0 is no line to compute."
line_calc {"w": 1e300, "h": 1e-300}
                          -> "No line: Conductor Losses came out as no number - the values given do not
                              make a line that can be computed."
receiver_budget {"temperature": -500}  -> "'temperature' is a temperature in C, above -273.15"
receiver_budget {"bandwidth": 1e300}   -> "'bandwidth' is a noise bandwidth in Hz, between 0 and 1e+18"
receiver_budget, a stage {"nf": -5}    -> "stage 1's 'nf' is a noise figure in dB, between 0 and 1e+06"
```

The last one mattered most: a noise figure below 0 dB is impossible, and it used to give a complete
budget that read as sound.

## B5: a count of nought

Three tools clamped it instead of minding it - `std::clamp(toInt(d), low, high)` - so `limit: 0` listed
one shape of four and said "1 listed of 4" as though that was what was asked, and `max_runs: 0` ran four.
Each refuses a count outside its range now, by name and with the range. All three checks are made
**before** anything else the tool does, so the answer is about the argument and not about the schematic
that happened to be in front.

## B3: a size that is no size

A rectangle's `size` is a `Pair` of expressions, and the check that judges a number against its unit is
deliberately skipped for lengths (a geometry's unit is the model's own, so there is no scale to judge
by). Three things are checked now whatever the unit, and for `Expression`, `Pair` and `Points`
properties alike: a constant that is not finite is refused, a constant beyond ±1e12 is refused as beyond
any geometry, and an expression that does not read is **said** at the edit - not refused, because it may
name a parameter the model has not got yet, and the solve turns it down in any case.

## B1, B2, B4, B8: four one-line fixes

- **B1.** `valuesAgainst()`'s "one number for one value, whatever it is named" is for the bare form,
  which arrives under an empty key. The condition now says so, and a misspelt name falls through to the
  answer's "No value's name is among the references (…)" - which was already written and never reached.
- **B2.** The "scanned" note is for a page with no text, so it is only given when no region narrowed the
  reading. A region that meets nothing says so instead, and points at `read_pdf` without one.
- **B4.** `updateGraphData()`, `setChanged(true, true)` and `viewport()->update()` moved out of the "a
  label differs" condition when N5 was added. They are back inside it, under a `wrote` flag that is set
  only where a label was actually written - so a re-show of an unchanged plot group leaves the `.dpl`
  alone (checked by its modification time and size over three shows).
- **B8.** `actionNamed()` matched an action's object name, and most `QAction`s have none - so `""`
  equalled the first one's. An empty name now names nothing, the match requires a non-empty object name,
  and the refusal says what to give.

## Tests

| test | suite | holds |
|---|---|---|
| `theNumericToolsRefuseWhatCannotBe` | `test_mcp_wishlist` | B6, B7: five impossible line inputs by name, a result that is no number, a temperature below absolute zero, a negative and an enormous bandwidth, a noise figure below 0 dB - and that an ordinary line and an ordinary budget still compute |
| `theToolsSayWhatTheyDidWithAnOddArgument` | " | B8 (and that no document is made), B2, B5 in all three tools, B3's refusal and its note, B1's three references |
| `aPlotGroupShownAgainLeavesItsDisplayAlone` | " | B4: the display's time and size unchanged over two further shows, a second apart, and no unsaved changes left on it |
| `aCsvKeepsItsHeaderAndSaysWhatItRenamed` | `test_data_import` | B9 (the header, every row, the note), B10 (the three names said, the map left alone), and that a plain CSV and a count line above the numbers read as before |

## Broken on purpose

`docs/feature_gaps/2026-10-24-hunt-b-fixes/breaks.py` holds 17, one or two for each fix, **all caught on
the first pass** - which is the A round's lesson paying off: every assertion checks *why* a call was
refused rather than that it failed, and every "said" check names the words it expects.

The breaks: the unnamed-reference fallback let in a named one; a narrowed page called scanned; a size
beyond any geometry taken; an expression that does not read not said; the display marked changed anyway;
each of the three counts clamped again; the permittivity left unchecked; no bound at all on a line's
input; a result that is no number handed back; absolute zero not minded; no two-sided bound on a level;
nothing naming the first action (two edits at once - the empty-name guard and the object-name guard,
since either alone still turns it down); the numbers beginning at the widest row; the widths not said;
and the renames keyed by the old name.

## One thing CI taught us on the way

The first shape of the B6/B7 checks handed `args` to the helpers - `mustBeAbove(args, "temperature", …)`
- and `check_tool_arguments` then reported five of `receiver_budget`'s arguments as read by nothing.
That check reads the keys a tool takes from the literal `args.value(QLatin1String("…"))` of its handler
and from the one `for (const char* key : {…})` loop it may have; a key handed to a helper, where it is a
`const char*` parameter, it cannot follow, and a second such loop leaves it with none. So the helpers
take the **value** now and the key stays a literal at the call:

```cpp
if (const QString why = mustBeAbove(args.value(QLatin1String("temperature")), "temperature", -273.15, …
```

which is the better shape anyway - a helper that judges a number rather than reaching into an object -
and leaves the check at 0 disagreements over all 133 tools.

## Not done

- **The note count on a wide CSV.** A file of many differing widths gets one note listing each width. It
  is the honest thing to say, but a file with dozens of distinct widths will say so at length.
- **`line_calc`'s values written with units.** `mustBeAbove()` judges only values given as numbers;
  `{"h": "0[mm]"}` still reaches `qucstrans`, where the result-level net catches what comes of it. Reading
  a unit here would mean parsing one in two places.
- **The `rxcalc` and `qucstrans` programs themselves** were not changed: the checks are on the Qucs-S
  side, so the same numbers typed into the dialogs behave as they did.
