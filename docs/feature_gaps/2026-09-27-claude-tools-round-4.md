# Feature gaps: Claude's Qucs-S tools, round 4

*27 September 2026 — Qucs-S 26.1.3.*

Claude built and simulated an emitter follower with the tools and wrote
down what worked, what got in the way, a wishlist and a top three:

1. a junction and floating-net check;
2. a structured operating point;
3. moving and hiding a component's text.

An addendum reported a silently shifted component line. Some of its
wishes were already there: the session ran a build from before round 3
([2026-09-26](2026-09-26-claude-tools-round-3.md)), which has
`add_painting` and one-diagram `export_image`. Each wish was checked
against the source; the tables say what was done.

## Friction points

| # | Friction | Was | Now |
|---|---|---|---|
| 1 | Silent wire crossing ("fix this one first") | `add_wire` drew a wire across another net's wire without a word, while its description spoke of refusing wires that run over another. A crossing is no junction, so the bias divider ended up as a net of its own, off the base. | A crossing is drawn, since crossings are part of drawing, but the result says so: *Look: the wires at 300, 130 cross without a junction: nets R1.2 and R3.2 are not connected there. To join a wire, end this one on it.* `connect`, `add_component` and `edit_component` say the same of what they bring about. The description now says what joins (a wire's end or bend) and what does not. |
| 2 | Hiding property text needed `set_schematic` | Rewriting a 50-field BJT line by hand. | `edit_component` (and `add_component`) take `shown` (`{"Is": false, "Bf": true}`) and `name_shown`. |
| 3 | Text placement had no tool | Hide the text, or move the part. | `text_at`: `[dx, dy]` from the part's centre, where its text begins. `get_schematic` gives each part's `text at` (and `name shown` when it is not). |
| 4 | Diagram overlap was easy to cause | x, y is the lower left corner, and nothing said when a diagram lay on another. | `add_diagram` and `edit_diagram` say what a diagram lies over (other diagrams by number, parts by name) and the free y below everything. `add_diagram` without x, y goes there. |
| 5 | Rotation reported as 1 for 0 | A DC source is made turned once ("fix historical flaw" in its constructor), and the summary gave the file's absolute rotation. | The tools count rotation from the type's own orientation, in `add_component`, `edit_component` and `get_schematic` alike. When the two differ, the summary also gives `rotation in the file`. |

## The wishlist

| Wish | Now |
|---|---|
| **Junction and connectivity check** (top 1) | `check_schematic`, and a `problems` count in the `get_schematic` summary. It gives *Simulation > Check Schematic*'s findings, each with its place and part, and the check gained what it lacked. **Warnings:** a wire's end or a pin on another net's wire mid-way (not joined); two nets' wires over each other; parts on no ground (floating), or in a subcircuit on no port; nets that reach ground only through capacitors or current sources (no DC path, so no operating point). **Notes, fine if meant:** wires of two nets crossing without a junction; a net label on one pin alone. Notes are not in the GUI's check: the shipped examples have 111 crossings. (`erc.h`: `wiring()`, `notes()`) |
| **Structured operating point** (top 2) | `simulate` with `operating_point: true` runs the DC operating point alone, as *Calculate DC bias* does, even for a transient-only schematic. It returns node voltages, branch currents and each device's operating quantities under its component. What they make plain is worked out beside them: `1/gm` (re of a BJT), `rpi = 1/gpi`, `beta = gm/gpi`, `ro = 1/go` or `1/gds`, `gm/gds`. The datasets are left as they were, and the next `simulate` runs the analyses. `get_dataset`'s operating point gains the same derived values. |
| **Moving and hiding text** (top 3) | See friction points 2 and 3. |
| Move a group | `move` takes names and dx, dy (in grid steps), and diagrams and paintings along. The wiring among the parts moves with them. The wires to the rest are drawn on to the pins' new places by the same healing that follows a keyboard move. A move that would join or split a net is refused and put back. One step to undo. |
| Auto-layout of a diagram, a free-area hint | See friction point 4. |
| Subcircuit support | `create_subcircuit` puts the named parts and the wiring among them into a new schematic beside this one. It gives a port to each net that also reaches the rest, and a ground inside for each pin on ground. One subcircuit instance takes their place, its pins joined to those nets by labels. The grounds and wire stubs the parts leave behind are cleared. One step to undo; the file stays. A symbol's name text lists the subcircuit's parameters (name, default, description, type, shown), and `edit_painting` sets them and the prefix. |
| Multiple measurements, ratios, expressions | A `get_dataset` variable may be an expression: `v(out)/v(in)`, `v(out)-v(in)`, `db(ac.v(out)/ac.v(in))`. It supports + − * / ^, parentheses and db, abs, mag, phase, real, imag, sqrt, log10, ln, exp and conj. It evaluates sample by sample in complex numbers for AC, each name taken from the analysis of the others. The result is then measured like any variable. Every curve's statistics gain `peak to peak`. (`dataset.h`: `evaluate()`) |
| AC and parameter-sweep helpers | `add_analysis`: `ac` (1 Hz to 100 MHz, log, 101 points unless given), `tran` (stop, points), `op`, or `sweep` of an analysis over a component's value (R2) or an equation's parameter. `plot` adds a diagram below the circuit: dB over a log frequency axis for AC, time for a transient. The block goes beside the other analyses. |
| Keep runs and diff them | `get_dataset` with `compare` (a `keep_as` name or a dataset file) puts the other run beside each variable. That gives its statistics and measurements over the same range, and the difference on this run's samples: the largest and where, the mean and the RMS. |
| Simulator differences in `describe_component_type` | The Vpulse note was there. Equations (Eqn, NutmegEq) now warn that a variable named like a node clashes with it under ngspice, silently. |
| Painting text rules | The painting tools say that in a text, `_x` and `_{xy}` are subscripts and `^` a superscript, and that there is no escape. A text with them gets a note. |
| Grounds as `GND.1` | Unnamed parts of which there are several each get a `ref` in the summary (`GND#1`, `GND#2`), and nets list their pins by it. `connect` and `add_wire` take `GND#2.1`. An ambiguous `GND.1` is refused with the refs to use. |
| A warning when a part lands on a wire or pin | `add_component` says which pin came down on what, and what it is joined to now. |
| A screenshot of one diagram or region | `screenshot` takes `diagram` or `region`: that part of the paper picture, sharp. |

## The addendum: a shifted component line

A `_BJT` line rewritten with one `"0" 0` pair too many loaded without a
word. The loader is positional, so every parameter after it moved one
slot: Tf became 0.5 s and the follower stopped following.

- **`set_schematic` refuses** a component line with more values than
  its type takes, naming the part and both counts: *R1 (R) has 7 property
  values, its type 6 properties*. The check needs no list of types: it
  compares the line's values with the properties the loaded part ended up
  with, so the types that take any number (subcircuits, libraries,
  equation-defined devices) pass as they should.
- A line with **fewer** values is taken, the rest at their defaults, and
  the result says so: a value left out mid-line shifts the rest the other
  way.
- **Not on opening a file:** the survey of the shipped examples found 30
  files whose lines carry trailing values of properties their types have
  since dropped (`.SW` with 7 values for 6, `Port` with 4 for 2). They load
  correctly, so a warning there would cry wolf. The refusal is where lines
  are written by hand, and it points old files to `open_document`.

## Found on the way

- **The first imaginary part of every complex vector was lost.** When a
  vector turned out complex, the reader filled "the values before it" with
  zero imaginary parts. On the first value there were none, so the fill
  did nothing and that value's imaginary part was not kept. Every AC
  variable's first frequency point was read with phase 0 and the wrong
  magnitude, and a two-point vector whose second point was real read as
  real altogether.
- **A pin with only a net label on it** was "connected to nothing" in the
  check, though the label joins it by name.
- **The shipped examples**, surveyed with the new checks: a VCO with a
  node that has no DC path; parts cut off where a library was missing.

## Tests

- **`test_erc`:** a schematic with a pin on another net's wire, a
  floating pair, a capacitor-isolated node, a crossing and a label on one
  pin. It checks the warnings, the notes kept out of the check, what
  `wiring()` tells, and that the working circuit is not mentioned.
- **`test_dataset`:** expressions (a ratio, a power, an AC ratio complex,
  its dB and phase, the analysis taken from the others) and their errors;
  the first imaginary part.
- **`test_qucs_control`:**
  - a wire drawn across another net's, said; a part's pin on a wire,
    said; `check_schematic` and the summary's counts; grounds by ref;
  - a real ngspice DC bias run of a diode through 1 kΩ from 5 V (node
    0.5–0.9 V, id 3–5 mA under D1), the datasets untouched and the next
    run its analyses;
  - text shown, hidden and moved, and undone; rotation from the type's
    orientation;
  - a shifted line refused, a short one noted;
  - diagrams placed below and overlaps said;
  - a group moved with its wiring, nets the same, undone;
  - AC with its dB plot, a transient beside it, a sweep of R1, four
    refusals;
  - a subcircuit made of two resistors (two ports, a ground inside,
    nothing left behind, `.SUBCKT` in the netlist, undone) and its
    parameters set;
  - expressions and a comparison with a kept run, peak to peak, a
    diagram's and a region's screenshot, the traps told;
  - the new tools in the 600 calls of odd arguments.
- **GUI monkey:** its fake Claude calls `check_schematic`, `move`,
  `add_analysis`, `create_subcircuit`, text edits, expressions with a
  comparison, partial screenshots and a shifted line. It was run under
  ASan/UBSan over 12 seeds.

Six of the fixes were broken on purpose to check that their tests fail,
and they did:

- the crossing warning;
- the floating check;
- the op-only run;
- hiding a property's text;
- the value-count refusal;
- the first imaginary part.
