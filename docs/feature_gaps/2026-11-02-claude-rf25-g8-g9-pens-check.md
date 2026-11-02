# RF 2.5D G8 and G9 and the viewers' pens, checked live on 2 November: a port's line marked, Claude's mouse on a layout and a slide

The reviewer's live check of G8 (ab18dfe), G9 (f717c20) and the viewers' pens (e2245e7, 8a58e2a), on the build of
16:06 (9603502): `~/Desktop/qucs-reviews/RF2p5D/2026-11-02-claude-rf25-g8-g9-pens-check.md`. G9 passed: the raw
MOM two-port, conducting and insulating silicon alike, passive to 100 GHz (it gave back 1.24 and 1.61). G8 was
flagged as asked: five warnings, the brief answer's too, the 4 % gain the de-embedding's. The pens' button and menu
read through `get_ui`, and `read_drawings` with nothing drawn said so. Two things were asked for and one noted:

| Feedback | Now |
|---|---|
| G9: raw MOM passive to 100 GHz, most returned 1; "second surface wave from" 75.27 GHz as information | As it should be |
| G9: the phase C GCPW with ground-referenced ports loses 1.8 % at 60 GHz (36 %); \|S21\| -0.77 dB there, "the port's 200 µm path now has its inductance and reflects - physical for such a port; a coplanar port avoids it" | No change: as the reviewer says |
| G9: the microstrip's eps_eff within 0.6 % of Kirschning and Jansen's at 1, 10 and 30 GHz, "the 1 GHz rise is copper's internal reactance" | No change |
| **G8, nit**: the port's reported line - 166 Ohm, eps_eff 5.36, 2374 dB/m at 100 GHz - "still printed as if valid; it could carry the same 'not trusted above 63.8 GHz' mark where it is shown" | **Marked where it is shown.** A port's line and its own discontinuity are taken at the top of the sweep; where its strips do not agree on one line there, every place that gives them says so: `rf25_solve`'s port (`taken at`, and `not to be trusted`: "the line, the shunt C and the series L, taken at 100 GHz: its strips do not agree on one line from 63.83 GHz"), the brief answer's too; the de-embedding's line for the port; the calibration's own line; and the tab's account of the solve |
| **Pens**: "`send_input` has no target for a layout or slide view ... so I cannot draw a stroke myself. Wanted: a layout/slide target for send_input, or a tool to draw a test stroke" | **`send_input` draws, as the user's mouse does.** `target` layout (points in µm on the cell shown, y up) or slide (points from the top left of `slide`, the one in sight unless given) - as the pen's strokes and `read_drawings` have them -, brought into view at the zoom shown, or with `pixels` the picture's. **`drag_through`**: a drag through points in turn, on any target - a ring, an arrow, a tick. With the pen taken up the answer reads each stroke drawn as `read_drawings` does (`drawn`), and says strokes taken away; the keys reach the view (Ctrl+Z undoes, Escape puts the pen down) |
| Note: the MOM's RLC model (`mom/mom_rlc.sch`) was fitted to raw data from before f717c20; the raw ports now carry their 305 µm paths' inductance (+242 pH), so its Ls of 117 pH would change if refitted | No change: the fit is the reviewer's own (numpy, `mom_fit.npy`), outside Qucs-S, which keeps nothing of it. The 242 pH is the answer's `series L` - taken at 100 GHz, where on this layout the strips do not agree, and now marked so |

## G8's mark

`mom_100_2p_cal_hiR.qrf25`, copied into the scratchpad and solved through `rf25_solve` before and after.
Before, the port's answer:

    "line dB/m": 2374.08, "line eps_eff": 5.36339, "line impedance": 165.94, "series L": 2.42367e-10,
    "shunt C": 8.07362e-16, "strips agree to": 58344100627.4, "strips differ by at most": 0.111, ...

and the lines "its reference plane moved 50 µm in from its face along a line of 165.9 Ohm, eps_eff 5.363", "At
100 GHz: the line 165.9 Ohm, eps_eff 5.363, 2.37e+03 dB/m; the port's own discontinuity 0.807 fF at its face and
242 pH at the port", "its feed a line of 165.9 Ohm, eps_eff 5.363 at 100 GHz" - each read as good, the warnings
twenty lines away.

After, with the S and the checks the same to the last digit (most returned 1.04 at 40.7 GHz, passive as solved):

    "taken at": 100000000000,
    "not to be trusted": "the line, the shunt C and the series L, taken at 100 GHz: its strips do not agree on one
                          line from 63.83 GHz"

- The de-embedding's line: "...along a line of 165.9 Ohm, eps_eff 5.363 - its own discontinuity and the line not to
  be trusted at 100 GHz: its strips do not agree on one line from 63.83 GHz."
- The calibration's: "At 100 GHz: the line 165.9 Ohm, eps_eff 5.363, 2.37e+03 dB/m; the port's own discontinuity
  0.807 fF at its face and 242 pH at the port - neither to be trusted: the strips do not agree on one line there."
- The tab's: "P1 de-embedded: ... its feed a line of 165.9 Ohm, eps_eff 5.363 at 100 GHz - not to be trusted there:
  its strips do not agree on one line from 63.83 GHz."

The mark goes by the frequency the values are taken at, not by the sweep: strips can stop agreeing at a middle
frequency and agree again at the top - shorter ones, solved beside the first, take over there - and then the values
given stand, and the warning says from where they did not. `strips agree to` (58.3 GHz) is the last frequency up to
which every one agrees; 63.83 GHz, the mark's and the warning's, the first at which one does not - the sweep's
step between them. Where the line is one given rather than the strips', the mark names only the shunt C and the
series L; where the reference plane stays at the face, the de-embedding's line names only the discontinuity, as no
line is moved along.

`PortDeembedding` carries where the strips first do not agree and whether they agree at the top (`offFrom`,
`topAgrees`), set once beside the agreement it already kept; the warning reads the first of them rather than
counting again.

## Claude's mouse on a layout and a slide

`send_input` reached a schematic's canvas, the window's parts and a dialog. It now reaches a layout's view and a
presentation's slides, in the measures of what is drawn on - the same as the strokes are held in and `read_drawings`
gives - so a stroke can be put exactly where a shape or a word is:

- **`target` layout**: the layout in front, or `path`; the points in µm on the cell shown, y up.
- **`target` slide**: the presentation in front, or `path`; the points from the top left of `slide` (from 1; the
  one in sight unless given).
- **Brought into view**, the zoom as it is: what is asked, the click and the drag's every point, centred where it
  is not all in sight (`LayoutView::setView`, the new `SlideView::centerOn`); refused where it cannot fit, with
  what zooms out (show_layout's 'region', show_pptx's 'zoom'). `pixels`: the picture's own.
- **`drag_through`**: `[[x, y], ...]` - up to 1000 points a drag from `click` goes through in turn, the button held,
  a step every four pixels or so of each leg, as a hand's drag gives a pen its points. On every target; not with
  `drag_to`, which keeps its eight steps.
- **The keys** go where the view has the focus, as the keyboard's do: Ctrl+Z undoes a stroke, Escape puts the pen
  down. (On slides a key the viewport does not take goes on to the slides, as Qt hands it up.)
- **The answer** reads each stroke the input drew, as `read_drawings` does (`drawn`: its gesture and what it marks),
  and says how many were taken away (an eraser's drag, an undo); the picture says what part of the cell or slide it
  shows, in its measures.
- **Asked about**: what the user is asked before it says how many points a drag goes through, and on which slide.

Drawn through the MCP server of this build, on the heater example's layout and a two-slide deck: a ring of 32
points round the top of the heater read as "a ring around 1 shape on 12/0"; a drag in pixels as a line; Ctrl+Z
"1 of the pen's stroke(s)" taken away; Escape the pen down; a click at y 2000 µm, out of sight, brought into view;
on the deck a ring round the title read as "around the paragraph Results of the run", and `slide` 2 scrolled to
and drawn on.

**What the tests found.** The slide's test drew nothing at first. `show_pptx` had just opened the deck: its view's
layout was still to come when the points were brought into pixels, and came before the mouse's events, which landed
off the slide. A view - and a part of the window, as before - is laid out first now (`laidOut`: posted layout
requests sent, the layouts from the window down activated).

`send_input`'s description, at 2003 characters, keeps under Claude Code's 2048; what the canvas and the window's
parts are given in it is said more shortly. `read_drawings`' description says that `send_input` draws.

## Tested

| Test | What it holds to |
|---|---|
| `whatItsStripsDoNotAgreeOnAtTheTopIsNotToBeTrusted` (`test_rf25_deembed`, new) | A line's ports de-embedded with strips not one line from 5 GHz nor at 10: where they first do not and that the top does not kept; the port moved along its feed's line said "its own discontinuity and the line not to be trusted at 10 GHz", the one at its face its discontinuity alone; warned of. Strips one line at the top though not at 5 GHz: nothing said of the values, the warning still. One line throughout: nothing |
| `aThirdStripSaysWhereTheStripsAreOneLine` (added to) | Strips of eight heights on 254 µm, not one line at the top: their line there "neither to be trusted"; the thin ones', one line, not; the chosen ones said so exactly when they do not agree at the top |
| `anRf25ModelIsSolvedFullWaveThroughItsTool` (`test_qucs_control`, added to) | A calibrated port's `taken at` 3 GHz, nothing said where its strips agree; a millimetre of strips on 635 µm of alumina at 42 GHz, 18 % from one line: `not to be trusted` word for word, and the tab's account of it marked in `what it did` |
| `rawInputIsSent` (added to) | What is asked before a drag through points on a slide: "dragged through 2 point(s) on slide 2" |
| `theToolsAreListed` | `send_input`'s description within 2048, every argument described |
| `theRawInputDrawsOnALayout` (`test_layout_doc`, new) | The pen up, the view on another part of the cell: a ring dragged through 24 points round the INV's output brought into view, drawn, and read in the answer as `read_drawings` reads it - round 1/0 and 2/0, the text OUT and the INV; pressed where asked and every point on the ring to 0.05 µm; a line in the picture's pixels; Ctrl+Z said taken away; Escape the pen down; a drag too long for the zoom, a layout not open and a point that is none refused, nothing drawn |
| `theRawInputDrawsOnASlide` (`test_pptx_doc`, new) | The pen up on a deck just opened: a ring of 36 points round slide 2's title drawn and read round "Slide 2", pressed where asked, every point on the ellipse to 5 %; a line on slide 3 scrolled to; Ctrl+Z taken away; Escape the pen down, a drag then drawing nothing; slide 4 of 3 refused for what it is, and two drags at once |

## Broken on purpose

`docs/feature_gaps/2026-11-02-claude-rf25-g8-g9-pens-check/breaks.py` holds twenty-five:

- **G8's mark:** whether the strips agree at the top not kept; where they first do not not kept; the top taken as
  not agreeing wherever any frequency does not; the warning only where the top does not agree; the de-embedding's
  line not marked; the line named where the plane stays at the face; the calibration's line not marked, and marked
  where its strips agree; the answer without `taken at`; its port not marked, and marked where the strips agree; the
  tab's account not marked.
- **The raw input:** a layout's points taken as pixels; a slide's from the first slide's top left; a layout's point
  out of sight not brought in; slides not scrolled to the slide asked; a drag through points not through them;
  slides laid out after their points are found; what the pen drew left out of the answer; every stroke said
  drawn, the earlier ones too; strokes taken away not said; a slide past the last, both drags and a point that is
  none taken; what is asked about a drag through points not saying so.

All twenty-five were caught. Two of the first twenty-six got past at first:

- **The slides' keys sent to their viewport** passed: Qt hands a key the viewport ignores on to its parent, the
  slides, so the code that sent them to the slides did nothing. It is gone, and so is its break.
- **A slide past the last not refused** passed: it was refused all the same - its point brought into view on a
  slide that is not there would not fit - but with the wrong reason. The test now holds to "'slide' is a slide's
  number, from 1 to 3".

Rebuilt after the breaks: the full ctest (152), the scenario script (97 checks) and the four lints green, nothing
written to the real cache.

## Under the sanitizers

Built Debug with ASan and UBSan, as CI's Linux job is, and solved with Eigen's sparse LU, each function alone:

| Function | Time |
|---|---|
| test_rf25_deembed `whatItsStripsDoNotAgreeOnAtTheTopIsNotToBeTrusted` | 8 s |
| test_qucs_control `anRf25ModelIsSolvedFullWaveThroughItsTool` | 85 s (48 s at f717c20): the thick strips' solve some 37 s of it |
| test_qucs_control `rawInputIsSent` | 5 s |
| test_layout_doc `theRawInputDrawsOnALayout` | 6 s |
| test_pptx_doc `theRawInputDrawsOnASlide` | 7 s |

On CI's Linux job at d2da15c `test_qucs_control` took 432 s of its 900 and `test_rf25_deembed` 584 of 900: some
75 s and 16 s more there leave both room, and main's kept tables (9a7225a) take the RF solves' cost down again.

## Not done

- **The strips a port's values at the top come from.** The de-embedding's line for a port, and the answer's
  `calibration strip` and `longer strip`, name the first strips solved (2460 and 4920 µm on the reviewer's MOM);
  where shorter ones were solved beside them, the top's line and discontinuity may be theirs (each frequency takes
  the longest that agree, else those that agree best). Noticed here, not changed.
- **The coarse-level preconditioner** for via-stitched structures, from phase E's list, still open; the phase E
  check stays unarchived for it.
- **A tool that draws without the mouse**, the reviewer's other way: `send_input` now does it through the same
  events a hand gives, which tests the pen whole.
