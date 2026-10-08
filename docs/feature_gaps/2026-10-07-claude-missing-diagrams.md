# Feature gaps: the missing diagrams of 3 October

*7–8 October 2026 — Qucs-S 26.1.7.*

The review (`Qucs-S_missing_diagrams_2026-10-03.md`, written at `935b184`)
listed seven diagram types worth adding and four lower-priority ones. It also
listed eight gaps beyond diagrams, and asked that each new diagram reach the
MCP tools. All of it is done here, on top of `91c15ce`, in the order the file
suggests. Its one prerequisite, `get_dialog` reading the dialog's variable
tables as rows of `false`, was fixed on 5 October.

Every item has a test, and every test was broken on purpose. Each break
changes one line of the new code, rebuilds, and runs the test that should
catch it: 329 breaks over the round. Each diagram was also probed through
`qucs-s --mcp-server`, mostly on a real ngspice 46 run, and its picture
exported and looked at. The server ran with its own HOME, settings, trash
and cache.

## The diagrams

| # | Review | Now |
|---|---|---|
| 1 | **Stacked panes**: N panes on one x axis, each its own y and y2 axis and traces; one zoom, one set of markers, a marker in one pane reading out in all. | **Stacked panes** (`stacked`): 2 to 8 panes on one x axis, each with its own left and right y axis. A trace's pane is saved with it (field 11 of the graph line; older files read pane 1). Zoom and limits are the shared x axis'. A marker placed in one pane reads every pane's traces at its x. The dialog has a *Panes* table: label, log, auto, from, to, step for each pane's axes. The tools take `panes` (a number or a list) and a trace's `pane`. |
| 2 | **Pole-zero map** of the `.PZ` analysis: poles ×, zeros ○, the jω axis, ζ and ωn guides; each root's value, ζ and Q. | **Pole-zero map** (`pole_zero`): the s-plane on one scale, poles as crosses and zeros as circles, the jω axis. Lines of constant ζ and circles of constant ωn are on by default. The cursor readout gives the point under it as a root: ωn, f, ζ and Q, and whether it is in the right half-plane. `get_dataset` measures `roots` without a diagram: each root's re, im, ωn, f, ζ, Q and whether it is in the left half-plane. |
| 3 | **Bode pair** (two linked panes, crossover and margins marked from the existing `phase_margin` and `gain_margin`), **Nichols** (gain in dB against phase, M and N contours, the critical point), **Nyquist** (−1 and the unit circle, the mirrored branch). | **Bode pair** (`bode`): a stacked diagram of two panes, magnitude in dB above phase, built on #1. The crossovers and both margins are marked from `ds::measure`'s own `phase_margin` and `gain_margin`. **Nichols chart** (`nichols`): open-loop gain in dB against phase, over the closed loop's M (magnitude) and N (phase) contours, the critical point marked. **Nyquist**: the polar diagram's `nyquist` option marks −1 and the unit circle, and `mirror` draws the negative-frequency branch. |
| 4 | **Contour / heatmap** with labelled iso-lines and a colour bar; spec limits as contours that show the region that passes. | **Contour map** (`contour`): a value over two swept parameters, the first along x and the second up y. The map is coloured in viridis, turbo or grey, with a colour bar beside it. Iso-lines are traced by marching squares, saddles included, at about `levels` round values, and labelled. `pass {min, max}` hatches what fails, draws the band's edges bold and gives the share that passes. Probed on a divider swept over two resistors (`.SW` inside `.SW`). |
| 5 | **Spectrum view**: dBc relative to the fundamental, harmonic numbers, THD, SFDR, SNR on the plot from `thd` and `fft`, a choice of window. | **Spectrum** (`spectrum`): a transient's windowed spectrum in dBc, as a line or as stems. The window is rectangular, Hann, Hamming, Blackman, Blackman-Harris or flat top. The harmonics are numbered on the plot, and THD, SFDR, SNR and SINAD are written in its corner. The settling at the start can be left out (`from`), and the fundamental can be given. `get_dataset` measures `spectrum` with the same numbers. |
| 6 | **Bathtub curve** beside the eye diagram, from the eye's folding and jitter. | **Bathtub curve** (`bathtub`): the bit error rate against the sampling instant, in UI from the eye's centre, on a log axis. The crossings are counted (drawn thin) and the dual-Dirac model is fitted to their tails on the Q scale. The opening at a target BER (1e-12) is marked, with RJ, DJ and TJ written. It folds the data exactly as the eye diagram does: the eye code's folding was made a shared `EyeDiagram::foldingOf`, the UI taken from the PRBS source the trace comes from. PAM4's three eyes each get a tub (solid, dashed, dotted). `get_dataset` measures `bathtub`, with `ber`. |
| 7 | **Sensitivity / tornado chart**: bars sorted by magnitude, from `.SENS`; the same view for Monte Carlo and corner runs. | **Tornado chart** (`tornado`): a bar for each trace, the largest on top. In `value` mode a bar goes from 0 to the trace's value at a point of its sweep (a sensitivity). In `spread` mode it goes from the lowest to the highest over the sweep (corner or Monte Carlo runs), with the value at the point marked. `parts: true` adds a bar for each part of the first trace's ngspice `.SENS` run. It uses `_scale` where the part has one (value × derivative: comparable across parts), else the derivative. The dialog has a button for the same. Probed on an RC's `.SENS`. |

### Lower priority

| Review | Now |
|---|---|
| Smith-chart overlays: stability, available-gain and noise circles, as Smith features. | **Circles on the Smith chart** (both kinds), of the two-port the traces' run is of, at one frequency (the middle of the sweep unless given). *Input* and *output stability* are dashed, with the unstable side shaded. *Available gain* is drawn at a level in dB, and *noise figure* at a level in dB from the run's NFmin, SOpt and Rn. A line under the chart gives the frequency, K, μ and "unconditionally stable" or "potentially unstable". The S-parameters are read as ngspice names them (`ac.s_2_1`, its noise `ac.nfmin` in dB) or as Qucsator does (`S[2,1]`, `Fmin` a ratio). The dialog takes "in, out, gain 12, noise 2" and a frequency; the tools take `smith_circles`. On the S-parameter template with noise, at 10.02 MHz, points on the 20 dB gain circle give Ga = 20.000 dB and points on the 3 dB noise circle give NF = 3.000 dB, computed independently from the run's numbers. |
| A constellation (IQ) plot. | **Constellation** (`constellation`): traces in pairs, I then Q, sampled once a symbol from an offset (or every sample), I along and Q up on one scale. With a modulation (BPSK, QPSK, 8PSK, 16QAM, 64QAM), the ideal points are drawn at the samples' rms, and the EVM, rms and peak, is measured. |
| A waterfall or spectrogram. | **Spectrogram** (`spectrogram`): a contour map's colours over a short-time Fourier transform. Time runs along and frequency up, with each segment's level in dB as colour, down to `range` dB below the loudest. Segment length, overlap and window are settable. The rows go up to a quarter past the highest frequency within range, not to Nyquist, so a chirp fills the chart rather than a sliver at its bottom. |
| A box plot of corner and Monte Carlo spreads. | **Box plot** (`box_plot`): a box for each trace. A trace of several curves gives each curve's value at an x (else its final value); a trace of one curve gives all its values. The box spans the quartiles (type 7, between the samples), with the median across it and the mean as a diamond. The whiskers are Tukey's (outliers as circles) or the full range. |

## Beyond diagrams

| # | Review | Now |
|---|---|---|
| 1 | **Delta markers**: Δx, Δy, 1/Δx from a reference marker; `add_marker relative_to`. | A marker can name another as its reference (*Edit Marker → Relative to*, `relative_to`). Its text then adds Δx, Δy and 1/Δx, and a dashed line joins the two. It follows its reference when that moves, and is saved (field 11 of the marker line). On the RC probe, a marker 2 ms after its reference read Δx 2 ms, Δy 85.6 mV and 1/Δx 500. |
| 2 | **Spec limits and masks**: horizontal, vertical and piecewise; the trace red where it crosses, pass/fail on the diagram and in the tools; with `tune`'s `hold` and `check_schematic`. | Upper and lower **limits** on a rect or stacked diagram: a level, or points (a mask's step is two points at one x), each with a label, an axis and a pane. They are drawn dashed, the trace is red where it is beyond them, and PASS or FAIL sits in the corner. `get_schematic` gives the verdict and each stretch beyond (from, to, worst, at). `tune`'s `hold` takes `{limits: n}`: a tuned value is kept only while that diagram passes. `check_schematic` warns of a diagram that fails, with the data it failed on. |
| 3 | **Overlaying a kept run** as a ghost behind the new one. | A trace can be a **ghost**: drawn faint (35 %) and behind the others, dimmed in the legend, with no axis label of its own. `add_trace` takes `run` (a run `keep_as` kept), and `edit_diagram` `overlay` adds a ghost of every trace from that run, or `null` takes them away. Diagram Properties has a *ghost* box for a trace. Probed on an RC whose run was kept and whose R was then changed: the kept run is drawn faint behind the new one. |
| 4 | **Cross-probing** (highest value): click a net, a pin or a part for its voltage, current or power in the diagram in front; a selected trace highlights its net. | *Simulation → Probe*. Click a net for its voltage, a pin for the current into it, or a part for its power; each click adds a trace to the selected diagram (else the last probed, else the first; a new one when there is none). An unnamed net is labelled first, since ngspice saves named nets. A current or a power the dataset lacks is saved by the next run (`.options savecurrents`, the schematic's `<ProbeSaves>`). Selecting a trace lights up its net or its part. The tool is `probe`. With real ngspice: `@r1[i]`, `@c1[p]` and `i(v1)`, 555 points each, and `net1` labelled and read at 1 V. |
| 5 | **Back-annotation following a cursor** in a transient or sweep. | *Simulation → Values at the Marker*: every named net is labelled with its voltage at the marker's time in a transient, or at its point of an AC or DC sweep. The labels follow the marker as it moves, drawn on the canvas and in prints. `add_marker annotate` does the same, and `get_schematic` lists the values while they are shown. |
| 6 | **Buses**. | *Insert → Bus*: a thick line named for the nets it gathers, `D[7:0]`, double-clicked to rename. It joins nothing by touching: its members are the nets named D7 to D0. *Check Schematic* says when a bus has no name, when a net ends on it unnamed or with a name that is no member's, and when ground meets it. The tool is `add_painting {type: bus}`, and the answer lists the members. |
| 7 | **LTspice `.asc` import**, its symbols onto parts and `.asy` onto subcircuits. | *File → Import LTspice Schematic…* and `import_netlist file: x.asc`. The `.asc` is read as UTF-16 or 8-bit. Each symbol's pins come from the `.asy` beside it or in a symbols folder, else from a table of the standard ones: res, cap, ind, voltage, current, bv, bi, the diodes, the bipolars, the MOSFETs. Rotations R0 to M270 are turned. Wires, flags (0 is ground) and T-joins make the nets, and `!` directives are kept. A symbol whose `.asy` is `Prefix X` becomes a subcircuit call to its `SpiceModel`; a symbol with no `.asy` and not in the table is left out and named. LTspice-only syntax is converted (µ, `SINE`, `.tran` with a stop time alone), and what is left out is said (Rser, a pin on nothing). The test imports an RC with a pulse and simulates it with ngspice: the peak is 0.157 V, as the netlist gives. |
| 8 | **Coloured wires** by signal type. | *View → Colour Wires by Net* (saved in the settings): a supply's wires red and thicker, ground's green, a signal's as before. A net is a supply by its name (VCC, VDD, +5V, a rail pattern), by a part's supply pin, or as a DC source's pin to ground (Vdc, or a constant `S4Q_V`). It applies to prints and exports too; `get_schematic` gives each wire's kind. |

## The MCP tools

- **Diagram types**: `add_diagram` and `edit_diagram` take `stacked`,
  `pole_zero`, `bode`, `nichols`, `spectrum`, `bathtub`, `contour`,
  `spectrogram`, `tornado`, `box_plot` and `constellation`. Each type's
  options go in an object of its name, and `get_schematic` gives what
  each one measured (`analyses`, `bars`, `boxes`, `pairs`,
  `smith_circles`, the limits' verdict). A marker on a type that has none
  is refused.
- **`get_dataset` measurements**: `roots`, `spectrum` and `bathtub` are
  new, and margins already existed. Also new:
  - `stability`, of a two-port's S-parameter with the other three beside
    it: K, |Δ| and μ at each frequency. Its value is the least μ. It says
    whether the two-port is unconditionally stable, and gives the ranges
    where it is not. With `at`, it gives each frequency's K, μ, |Δ|, the
    maximum available gain (or the maximum stable gain when K ≤ 1), and
    both stability circles.
  - `evm`, of an I with its Q (`evm: {q, symbol_period, offset,
    modulation}`): the constellation's sampling, shared as
    `ConstellationDiagram::sampled`.
  - `distribution` gives the box plot's quartiles, whiskers and outliers.
- **`export_image`'s `diagram`** already existed. Each new type exports
  through it, and the probes' pictures were made with it.

## Found on the way

In what was there before:

- **An S-parameter run headless wrote no S-parameters.** The SP block
  chose its vectors by the simulator stored in the settings, not the one
  in use. A fresh settings folder, `--ngspice` or a tool's own simulator
  stores none, so only the equations' variables (`S21_dB`) were written,
  and `ac.s_2_1` never reached the dataset. The simulation block's colour
  read the same stored setting. Both now read the simulator in use.
- **ngspice 46 spells the S-parameters `s_1_1`.** ngspice's raw file puts
  `v(...)` around a vector of a voltage's type, and 46 types them
  "s-param". Twelve shipped examples trace `ac.v(s_1_1)`, the S-parameter
  template's Smith chart among them, and showed nothing. A trace's
  variable is now also tried spelt the other way: `ac.v(x)` as `ac.x`,
  besides the existing `ac.x` as `ac.v(x)`. This holds in the text and
  binary dataset readers and in `get_dataset`. The template's Smith chart
  draws its 100 points again.
- **Every new diagram type resized as a square**: the resize kept every
  diagram square unless its name was one of a list (Cartesian, eye,
  timing, table, locus). It now keeps only polar and Smith charts square.

In the new code, caught by its tests and probes before it was committed:

- μ computed with the ports swapped: |S11 − Δ·S22*| where Edwards and
  Sinsky's denominator is |S22 − Δ·S11*|. The textbook two-port's μ
  caught it.
- A tornado chart's values freed at layout (a graph that draws no curve
  had its data dropped), and the next paint crashed (exit −11).
- What a contour or tornado chart draws beyond its frame given at the
  origin, not at the diagram.
- The marker's values drawn into exports only, never on the canvas, which
  draws the scene it holds during gestures.
- A constellation's I and Q axes on one range but rounded to different
  steps: a circle drawn as an ellipse.
- A colour bar's end reading "−29m" for −0.029 dB; it now gives three
  significant digits.

## Tests

`test_stacked_diagram` (9 tests) and `test_diagram_tools` (24) are new.
The second's are named for the items: stackedPanes,
crossProbing, probingAnUnnamedNet, deltaMarkers, specLimits,
aTuneHoldKeepsTheLimits, poleZeroMap, loopDiagrams, spectrumView,
bathtubCurve, resizingKeepsItsShape, contourMap, tornadoChart,
aMarkerInAFileOnADiagramWithoutMarkers, keptRunOverlay,
valuesAtTheMarker, buses, ltspiceImport, colouredWires, spectrogram,
boxPlot, constellation, smithCircles, sParameterRunWritesItsS.
`test_binary_dataset` checks the other spelling in both readers. Where
there is a number to work out independently, the tests check that, not
the code's own output:

- the Smith circles: points on each circle give |Γin| = 1, |Γout| = 1,
  Ga = 10 dB and NF = 2 dB by their definitions;
- the spectrum: a harmonic's amplitude and frequency;
- the contour: iso-lines through a hand-made grid's saddles.

The pictures are checked at the pixels where something must be drawn.

The 329 breaks are in a script of the session's (`breaks6.py`, not in
the repository); the last full run of
them, on the finished code, caught 322. Of the other seven, one no longer
built (its line had moved), and six showed tests too weak. Those six
tests were made stronger, and all seven were then caught: 329 of 329.

- the probe: a named diagram that is not the last one probed;
- the limits: a mask ending in its step;
- the Nichols chart: a loop gain past −360°, which must keep unwrapping;
- the Nyquist marks: the ring around −1, apart from its cross;
- the tornado chart: its labels, looked for after a full layout (a
  resize had dropped them, so the check saw none);
- `evm`: the range kept to on I and Q both.

Earlier, as each item was built, a break its test did not catch meant a
test too weak, and the test was made stronger:

- the spectrum: the amplitude at its bin, the df after `from`, the line's
  pixel;
- the contour: a vertical crossing and the saddles of a hand-made grid;
- the overlay: the live trace's colour, and a ghost of the same run drawn
  behind it;
- the LTspice import: a tap flag, a subcircuit's flagged pins, an NMOS;
- the box plot and the constellation: a value between samples, and the
  first point.

Four breaks changed nothing that can be seen from outside, and were
removed or replaced:

- a contour's "up y" became "a map's range, not its grid's";
- the Smith chart's line for a null frequency was redundant, since the
  value reader already gives null as NaN, and became "a frequency of 0
  or below taken";
- "a trace of one sweep";
- a scene key for coloured wires.

`ctest`: 95 of 95 pass, with its own HOME, settings, trash and cache.
One failed first: `add_diagram`'s description had grown past the 2,048
characters Claude Code keeps of a tool's. Its list of types now names
them and points to each one's own option. `scripts/mcp-e2e-scenarios.py`
on the build: 97 checks, none failed.

## Limits

- The Smith circles are of ports 1 and 2. On a network of more ports,
  `get_dataset` says so: the others are terminated as the run had them.
- `evm` and the constellation take real I and Q signals (a transient's),
  not a complex baseband vector.
- The LTspice import maps the standard symbols and those whose `.asy` it
  is given. `.lib` and `.include` directives are kept as written; a file
  of an LTspice install is not looked for, so they may need pointing at
  it.
- The bus is a drawing with a name. Its members are joined by their
  labels, as before; there is no bus-wide net type in the netlist.
