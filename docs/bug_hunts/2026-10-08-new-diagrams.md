# Bug hunt: the new diagrams and what came with them

*8 October 2026 - Qucs-S 26.1.7, `68c9279` (the Release build and the ASan/UBSan build of it).
06:55 to 07:50, the findings written up as they came. All fixed in `04e762a`.*

This hunt covers what the round of `503e67f` added (the review of 3 October, "the missing
diagrams"):
- **The new diagram types**: stacked panes, the pole-zero map, Bode and Nichols, the
  Nyquist marks, the spectrum view, the bathtub curve, the contour map and the spectrogram,
  the tornado chart, the box plot, the constellation, and the Smith chart's circles.
- **What came with them**: delta markers, spec limits, a kept run's ghost, Simulation >
  Probe, Values at the Marker, buses, File > Import LTspice Schematic, and Colour Wires by
  Net.
- **Their reach**: through the tools (`add_diagram`, `edit_diagram`, `add_marker`,
  `probe`, `get_dataset`), the Diagram Properties dialog, and the saved file.

It ran in the foreground on macOS arm64 with ngspice 46. Each `qucs-s --mcp-server` had
its own workspace, settings, HOME, trash and cache in a scratch folder. Nothing was written
to the real cache or workspace (checked with `find -newer` against a marker). Methods:

1. **Probes through the tools**, p1 to p81, files beside this report. `common.py` copies
   the shipped RC_filter_FFT example into a project and runs it; `mcp.py` is the client of
   the earlier hunts. Most probes ran on the ASan build. Every crash was checked again on the
   Release build.
2. **Numbers by hand** where a diagram measures something: the poles, the phase margin, a
   contour's passing share, the quartiles, the EVM, the THD, and the values at a marker on an
   AC sweep. Each was computed with numpy from the dataset's own values.
3. **Fuzzers on the ASan build**:
   - `scripts/ci/fuzz-sch.py` over a seed schematic holding every new type, with markers
     (a delta pair), limits and a bus (`seeds/`): 300 mutants without its dataset, 300 with
     it, then 400 more;
   - an LTspice fuzzer of this hunt's (`p9`): 2,250 damaged `.asc` files through
     `import_netlist` and `get_netlist`;
   - 27 hand-made edits of the new fields in a saved file (`p7`).
4. **Reading** the code behind each finding, for its cause.

## Summary

| | Finding | Since |
|---|---|---|
| **F1** | A marker on a diagram whose log x axis starts at 0 (a linear AC sweep from 0 Hz, an FFT's spectrum, a Bode diagram of either) crashes Qucs-S | not determined |
| **F2** | A spectrogram whose segment is far shorter than the sampling takes time growing as 1/segment: a schematic that keeps one cannot be opened | `503e67f` |
| **F3** | The LTspice import does not bound coordinates: a part or pin near 2^31 overflows (Debug builds abort, Release wraps) | `503e67f` |
| **A1** | The tornado chart's parts leave out every part with an underscore in its name, without a word, and find none in ngspice 46's AC sensitivity | `503e67f` |
| **A2** | A limit on a pane that was taken away is no longer checked, and the diagram says PASS | `503e67f` |
| **A3** | The spectrum view takes f0 as the nearest bin and numbers neighbouring bins as harmonics: a square wave's 2nd "at 1800 Hz, -45.6 dBc", a pure sine's THD 0.2 % (Hann) to 5.5 % | `503e67f` |
| **A4** | The eye diagram's width at BER 1e-12 and the bathtub's opening at 1e-12 disagree on one signal (0 against 0.57 UI) | `503e67f` (the bathtub) |
| **A5** | Probing a part's second pin plots the current into its first: the sign wrong, labelled as the pin's | `503e67f` |
| **A6** | A delta marker placed on an earlier trace measures from the wrong marker (`relative_to` read after the renumbering) | `503e67f` |
| B1 | An analysis added beside another of its kind (an .AC beside an .FFT) renames every vector: every diagram goes blank, and `get_dataset` refuses the names its description gives | before |
| B2 | Deleting a delta marker's reference leaves the Δ lines in the other marker's text | `503e67f` |
| B3 | Probe names an unnamed net `net1`, a name `get_schematic` gives another net: two nets called `net1` | `503e67f` |
| B4 | The Smith chart's circles at a frequency outside the sweep are drawn at its end, without a word | `503e67f` |
| B5 | `add_diagram`'s trace objects refuse `run`, which `add_trace` takes and its description promises | `503e67f` |
| B6 | `probe` of a trace already shown says "has data: false" and "Simulate to see it" while it shows 1425 points | `503e67f` |
| B7 | Types made for one domain take another without a word: a spectrogram cuts an AC sweep as time; Bode, Nichols and the pole-zero map draw a transient | `503e67f` |
| B8 | Colour Wires by Net: the rail names it lists (+5V, -12V, 3V3, V+) cannot name a net; VDD_IO is not a supply, VIN is | `503e67f` |
| B9 | A marker's x is saved to 6 digits: it moves on save, reopen and undo, and at an edge its reading flips (0 to 1) | before (upstream) |
| B10 | A stacked diagram puts seconds and hertz on one x axis, and its marker reads one at the other's x | `503e67f` |
| B11 | A square wave is drawn with a slope before each edge (points near the last drawn skipped whatever their y) | not determined |
| B12 | Probing a transistor's emitter (`@q[ie]`, which ngspice does not write) makes every run of the schematic fail | `503e67f` |
| B13 | Under Qucsator, probing a net is refused: "the netlist could not be written", which it can | `503e67f` |
| B14 | The LTspice import drops `.step`, `.meas`, `.four` and `.wave` without a word: a stepped run becomes one | `503e67f` |
| B15 | An LTspice capacitor or inductor with its database ratings (`V=50 Irms=1`, `Ipk=1 mfg=...`) imports, said to be left out, but kept: ngspice refuses the netlist | `503e67f` |
| B16 | A Nichols chart's M/N grid grows with the axis range: one diagram of an FFT's spectrum is a 105 MB SVG | `503e67f` |
| N1-N24 | Minor: below | |

There were no crashes in the schematic fuzzer's 1,000 mutants, the 27 file edits apart from
F2's, or any probe apart from F1's and F3's. The ASan build reported nothing except F1's null
load and F3's overflows.

All 25 findings and the 24 minor notes are fixed in `04e762a` (each says how, below; *The fixes,
checked* says how they were tested).

## F. Crashes and hangs

### F1. A marker on a log x axis that starts at 0 crashes Qucs-S

A marker placed on a diagram whose x axis is logarithmic, over data whose first x is 0,
kills the program (SIGSEGV, exit -11, the Release build). This is the plainest case of a
Bode diagram: a linear AC sweep from 0 Hz. Probes `p34`, `p36` and
`p38`:

| diagram | data | marker |
|---|---|---|
| bode | RC, `.AC` lin from 0 Hz | crash |
| bode | the FFT's spectrum (from 0 Hz), or a transient | crash |
| rect, x log | the FFT's spectrum | crash |
| stacked, x log | the FFT's spectrum | crash |
| stacked, x linear | the same | placed |
| bode | `.AC` log from 10 Hz (or 100 Hz) | placed |

The ASan build gives the cause:

```
stackeddiagram.cpp:214:17: runtime error: load of null pointer of type 'const double'
ERROR: AddressSanitizer: SEGV on unknown address 0x000000000000
  #0 StackedDiagram::calcCoordinate(...)
  #1 Marker::initText(int)
  #2 Marker::Marker(Graph*, int, int, int)
  #5 QucsControl::addMarker(QJsonObject const&)
```

`Marker::initText` (`diagrams/marker.cpp:153`) walks the trace's x values and calls
`calcCoordinate` with a pointer into them. The pointer is null: the graph has no x points.
The likely cause is that a graph that draws nothing on the log axis (every point at or left
of 0) has its data dropped, the memory saving of the scale round, while the marker still
walks the data. Placing the marker by hand in the window takes the same path (the
`Marker` constructor). The crash is not reached by re-simulating or reopening a file that
already holds a marker (`p37`). It is reached by placing a new marker, and by moving one
(`p39`): a marker placed on an FFT's spectrum while the x axis is linear survives the axis
being made logarithmic, and the program dies when the marker is then moved (rect and
stacked; a polar diagram lives). In the window, that is a spectrum plot set to log x and
its marker dragged. When it began was not determined.

The diagram itself draws nothing (`p72`, `files/f1_bode_invalid.png`). The Bode of the
FFT's spectrum is two empty panes, its axis labelled `ngspice/ac.v(out) <invalid>`: the
one point at 0 Hz, which a log axis cannot place, invalidates the whole trace. Yet
`add_diagram` answers that the trace has 1025 points, with no note. That is the state the
marker then crashes on, and it is the first thing a user sees of a Bode diagram of a
linear sweep from 0 Hz.

**Fix**: in `Marker::initText`, return (an invalid marker) when the graph's x axis or its
`cPointsY` is null; and give the reason in `add_marker`'s answer. Leave out the points at or below 0 on a log axis
instead of the whole trace, and say so.

**Fixed in `04e762a`.** A log axis leaves out of its range the points at or below 0 of a
graph that has points above 0 (`Diagram::getAxisLimits`, `logLeavesOut`): each is drawn off
the axis, the rest laid out. A Bode diagram of a linear sweep from 0 Hz, and a spectrum on a
log x, are drawn, and `add_diagram`'s trace says "left off the log axis: 1 point(s) at or
below 0". A graph all below 0 is laid out mirrored, as before. `Marker::initText` makes a
marker on a graph its diagram does not draw (no y: an axis given through 0) invalid instead of
walking the null pointer, and `add_marker` refuses it, "is not drawn (its diagram cannot lay
out a log axis over it ...)", as the trace then says "not drawn". p34, p36, p38 and p39 place
and move every marker, on the Release build and the ASan one.

### F2. A spectrogram with a very short segment hangs, also when its file is opened

`add_diagram {type: spectrogram, spectrogram: {segment: s}}` on the RC's 10 ms run
(`p3`, Release build):

| segment | time | answer |
|---|---|---|
| 1e-9 s | 4.0 s | "fewer than two segments of 1e-09 s: give a shorter segment" |
| 1e-10 s | 40.3 s | the same |
| 1e-11 s | no answer after 90 s | (killed) |

The loop over the segments (`diagrams/spectrogramdiagram.cpp:107`) stops at
`kMostColumns` columns, but counts only segments that gave a spectrum. A segment holding
fewer samples than a spectrum needs gives none and is skipped uncounted. So the loop walks
the whole run in steps of half a segment: 10 ms / 5e-12 s is 2·10⁹ steps. The segment is
saved with the diagram. `files/spectrogram_seg_1e-12.sch` is the seed with its segment
set to 1e-12 s, and loading and rendering it (`qucs-s -p`, the ASan build) did not finish in 60 s (`p7`). In
the window, opening that schematic would hang Qucs-S.

The answer is wrong as well: the segment is too *short*, not too long.

**Fix**: refuse a segment shorter than a few sample intervals (in `segmentOf`, the tool
and the dialog), or count every step toward the cap; and say "give a longer segment".

**Fixed in `04e762a`.** `SpectrogramDiagram::gridFor` refuses a segment that holds fewer
than 8 of the run's samples (their mean interval) and names the shortest it takes: "a segment
of 1e-09 s holds fewer than 8 of the run's samples (7.02247e-06 s apart on average): give a
longer segment, 5.61798e-05 s or more". Every step toward the 1024 columns counts, whether its
segment gives a spectrum or not, and the step is lengthened so that the 1024 cover the whole
run (they were the first 1024 steps). The "fewer than two segments" message names both ways
out. p3 answers at once for all three segments; `files/spectrogram_seg_1e-12.sch`, with the
seed's dataset beside it, renders in 0.4 s.

### F3. The LTspice import does not bound coordinates

From 2,250 damaged `.asc` files (`p9`, seeds 1 to 3) on the ASan build, three killed the
server, and UBSan reported signed overflows:

```
ltspiceimport.cpp:172:57: runtime error: signed integer overflow: -2147483616 - 80 cannot be represented in type 'int'
Fatal: ASSERT failure in QCheckedInt ... "Overflow in operator+", qcheckedint_impl.h, line 70
```

`files/qcheckedint_abort.asc` is the test's sample with `SYMBOL nmos 800 2147483647 R0`.
The Release build imports it and lives, with the part at a wrapped coordinate. The Debug
build aborts in Qt's checked arithmetic (a QRect of the placed part). `onSegment`
(`ltspiceimport.cpp:172`) subtracts in `int` before widening:
`qint64(b.x() - a.x())`. A `.asy` pin at 2147483647, -2147483648 aborts the same way
(`p31`).

**Fix**: refuse coordinates beyond what a schematic holds (Qucs-S keeps ±8,388,608, as the
probe's own message shows), and widen before subtracting in `onSegment`.

**Fixed in `04e762a`.** Every coordinate of an `.asc` (WIRE, FLAG, SYMBOL) and of an
`.asy` pin is read as a whole number within ±8,388,608 (`Schematic::ModelLimit`). Else the
import is refused with its line: "line 35 (SYMBOL nmos 800 2147483647 R0) has a coordinate
that is not a whole number within 8388608 of the origin"; a symbol with a pin beyond it is
not used, and the refusal says why. `onSegment` widens before it subtracts. Both files of
`files/` are refused on the ASan build with no report.

## A. Results wrong without an error

### A1. The tornado chart's parts leave out parts named with an underscore

`parts: true` (and the dialog's button, through the same
`TornadoDiagram::sensitivityParts`, `diagrams/tornadodiagram.cpp:73`) takes a name
without `_` that has `<name>_...` beside it for a part. On the shipped
`sensitivityACandDC.sch` with R2 renamed R_load (`p19`), the dataset has `r_load`,
`r_load_scale` and the rest. The chart shows v1 (0.909) and r1_scale (-0.413), but not
r_load_scale (+0.413), which moves the output exactly as much as R1. It isn't among the
bars "of nothing" or "not shown" either, and nothing says it is missing. R_load, C_in and
L_out are common names.

In ngspice 46's AC sensitivity (`.SENS_AC`) the parts are written `ac.v(r1)`,
`ac.v(r1_scale)`. The same rule then cuts `ac.v(r1` before the `_`, and finds no part
(`p18`): "has no part of a sensitivity run over frequency (ngspice's .SENS: r1, r1_scale,
v1, ...)", for a dataset that has them. It is the `v(...)` spelling of `503e67f`'s "Found on
the way" again.

**Fix**: read the parts from the names that end in `_scale` (or the known parameter
suffixes) instead of splitting at the first `_`, and take the `v(...)` off first.

**Fixed in `04e762a`.** `sensitivityParts` takes each name as the run's own (its analysis's
prefix, and the `v(...)` of ngspice 46's AC sensitivity, taken off), and a part is a name with
parameters beside it (`r_load` with `r_load_scale`), whatever it holds. p19 shows
r_load_scale (+0.413) beside r1_scale; p18 finds r1, r2 and c1 in the AC run.

### A2. A limit on a pane taken away passes

A stacked diagram of 4 panes, `tran.v(in)` (up to 1 V) in pane 4, an upper limit of 0.9 on
pane 4: FAIL, with its stretches (`p5`). `edit_diagram {panes: 2}` moves the trace into
pane 2 (`stackeddiagram.cpp:119` clamps a graph's pane), but leaves the limit on pane 4.
`limitAxis` (`:168`) gives no axis for it, so the limit is skipped and not drawn (`files/a2_orphaned_limit_pass_b11_shoulder.png`: PASS over a v(in) of 1 V). The verdict is now
`{"pass": true}`, `check_schematic` says nothing, and the limit stays in the file
(`p4`: `"limits": [{"pane": 4, "upper": 0.9}]` after reopening). `tune` with `hold [{measure: {limits: n}, max: 0}]`
(`p81`) measures every value 0.1 beyond and keeps none while the pane is there. After the
pane is taken away it measures 0 beyond, and keeps every value.

**Fix**: clamp a limit's pane as a graph's is (or refuse to take away a pane that has
one); and count a limit with no axis as a failure to check, not a pass.

**Fixed in `04e762a`.** Taking panes away moves a limit's pane with the traces'
(`StackedDiagram::setPaneCount`), and `limitAxis` reads a pane past the panes (a file's) as the
last, as a graph's is read. A limit no trace with data is drawn against checks nothing, and is
not counted as a pass (`limits::unchecked`): the verdict is `"pass": false, "unchecked": [n]`
with a note, the corner says UNCHECKED (amber), Check Schematic warns "its upper limit 1
checks nothing", and `tune`'s hold refuses it ("diagram 3's limit 1 checks no trace"). p5 and
p81 FAIL after the panes are taken away, and keep no value.

### A3. The spectrum view's f0 and harmonics are a bin off

The RC example's 1 kHz square (`p1`, `p24`): the spectrum's harmonics are at 1000, 1800,
3000, 3800, 5000, 5800 ... Hz. The 2nd, 4th, 6th and 8th are each reported 200 Hz below
where they are. On the ideal square of `tran.v(in)` the "2nd harmonic" is -45.6 dBc at 1800
Hz. The 2 kHz line itself is at -318 dBc, since a square has no even harmonics. With `from:
0.002` the bins are 125 Hz wide and the "2nd" is at 1875 Hz.

`sp::analyse` (`spectrum.cpp:198`) takes for harmonic *h* the strongest bin within the
window's lobe of *h*·f0, and reports that bin's frequency and level. When the harmonic is
absent, that bin is leakage from elsewhere, and it is drawn and numbered as the harmonic.
The picture is `files/a3_spectrum_harmonics.png`: the circles 2, 4, 6 and 8 sit 200 Hz left of their harmonics, two of them off the curve. The THD hardly moves: 42.90 % against 42.88 % by hand for harmonics 2 to 9, since the
leakage is small next to the odd harmonics. But the numbers printed on the plot, and
`get_dataset`'s `spectrum`, name a 2nd harmonic that is not there.

A record that is not a whole number of periods makes it worse (`p78`). A pure 1 kHz sine of
10.5 periods (an imported `.npy`, 20,000 samples) gives these readings:
- **f0**: 952.4 Hz with every window but flat top, which gives 1047.7 Hz. That is the
  nearest bin (95.2 Hz wide), half a bin off, not interpolated between bins.
- **Where the harmonics are sought**: at whole multiples of that bin, which drift from the
  true harmonics as *h* grows. The 9th is sought at 8,571 Hz for 9,000 Hz, 4.5 bins away,
  beyond the lobe searched.
- **THD of the clean sine**: 5.5 % (rectangular), 0.86 % (Hamming), 0.20 % (Hann), 0.004 %
  (Blackman-Harris). The older `thd` measurement takes the curve's own frequency from its
  crossings, and so does not drift.

**Fix**: take the peak only when it stands above its neighbours (a local maximum at most a
bin or so from *h*·f0). Take f0 from the peak interpolated between bins (or from the crossings, as `thd` does), and seek each harmonic at *h* times it. Otherwise report the level of the bin at *h*·f0, and its frequency as *h*·f0.

**Fixed in `04e762a`.** `sp::analyse` reads the fundamental between the bins (a parabola
through the logarithms of its bin and the two beside it) and seeks harmonic *h* at *h* times
it. A harmonic is a peak: a bin above both beside it, within 0.75 of a bin of *h*·f0 and 10 dB
above the floor (the median bin). Else none is there, and the bin at *h*·f0 says how little,
at *h*·f0. p24: the square's 2nd at 2000 Hz, -344 dBc (from 2 ms), every even one at its
multiple, the THD as it was (42.90 %). p78: f0 1000.0 Hz with every window but the
rectangular (995.7 Hz); the clean sine's THD 0.022 % (Hann), 0.27 % (Hamming), 0.008 %
(Blackman), 0.0008 % (Blackman-Harris) - the rectangular window's 2.4 % is its leakage.

### A4. Two widths of one eye at BER 1e-12 disagree

On the shipped `PRBS_eye_diagram.sch` (`p20`), for one signal, `tran.v(rx)`:
- **The eye diagram** (and `get_dataset eye`): "width at BER 1e-12": **0**, a closed eye.
- **The bathtub** (and `get_dataset bathtub`): "opening" at 1e-12: **0.572 UI**. Its fit
  gives RJ 1.35 ps rms, DJ 24.4 ps and TJ 42.8 ps of a 100 ps UI.

The eye's width appears to take the whole crossing spread (10.6 ps rms) as random jitter,
which at 1e-12 (±7 σ) is wider than the UI. The bathtub fits the dual-Dirac model to the
tails. Both answers are labelled the same, and one of them is wrong for this signal. Measured from 2 ns for both, the example's own `from` (`p79`), it is still 0 against
0.531 UI, so the settling is not the cause.

**Fix**: have the eye report the bathtub's figure (`EyeDiagram::foldingOf` is already
shared), or name its own as the Gaussian upper bound it is.

**Fixed in `04e762a`.** The eye's width at BER 1e-12 is the bathtub's opening there, its
dual-Dirac fit (`eye::bathtubOf`, shared); a UI less 14.069 rms jitters stays only where there
is no bathtub. p20 and p79: 0.5649 UI from both.

### A5. Probing a part's second pin plots the current into its first

Probe's pin is "the current into it". On the RC (`p52`), `probe R1.1` adds
`tran.@r1[i]`, ngspice's current from R1's first node to its second: into pin 1, +8.187 µA
at 0.2 ms, as (v(in) - v(out)) / 100 kΩ gives by hand. `probe R1.2` answers "The diagram
shows ngspice/tran.@r1[i] already": the same trace, with the same sign. The current into
pin 2 is -8.187 µA. A click on a part's second pin therefore shows the current flowing out
of it, labelled as the current into it. Every two-pin part does the same (`p8`, `p56`): pin 1 and pin 2
of C1 both give `@c1[i]`, of V1 `i(v1)`, of D1 `@d1[id]`, of L1 `i(l1)`, of I1 `@i1[current]`.
A MOSFET's three pins give `ig`, `id` and `is`, each its own.

**Fix**: for a two-pin part's second pin, add the negated vector (an equation, `-@r1[i]`, as
the probe already adds NutmegEq for expressions), named for the pin.

**Fixed in `04e762a`.** `probe::vectorOf` finds the terminal of the device the pin's node
is at. A two-terminal device's current is into its first terminal, so its second gets the
negative, `-@r1[i]` (a bipolar's emitter `-(@q1[ic] + @q1[ib])`: B12). Such a current is
computed by a NutmegEq after the analysis - one there takes the equation, else a new one goes
beside the analysis - named after the pin, `r1_pin2`, its vectors saved by the next run. The
trace is `tran.i(r1_pin2)`; ngspice writes a vector computed of vectors of no type as the name
alone, and both dataset readers and `get_dataset` take either spelling. A controlled source's
control input is refused (no current flows into it); under Xyce, where no NutmegEq runs, the
probe says to probe the other pin. p52: into R1.2 -8.187 µA at 0.2 ms, into R1.1 +8.187 µA.

### A6. A delta marker measures from the wrong marker when it is placed on an earlier trace

Markers are numbered trace by trace: those on trace 1 first, then trace 2's. A new marker
on an earlier trace takes a lower number and shifts the others, and its `relative_to` is
read after that shift (`p63` to `p65`). Markers 1 (2 ms) and 2 (6 ms) on trace 2, then
`add_marker {trace: 1, at: 0.004, relative_to: 2}`, meaning the one at 6 ms as
`get_schematic` numbered it. The new marker becomes marker 1, the one at 6 ms becomes 3, and
the delta is taken from the one at 2 ms: Δx = +2 ms, where -2 ms was asked for, with no
error. With a single marker on trace 2, `relative_to: 1` is refused as "itself". In a saved
file the numbering after reopening is trace by trace too, so the order the markers were made
in is not kept either.

**Fix**: resolve `relative_to` before the new marker is inserted (and keep a reference as
a pointer, not a number, until saving).

**Fixed in `04e762a`.** `add_marker` resolves `relative_to` before the new marker is
placed, by the numbers `get_schematic` gave (`referenceOf`). p65: Δx = -2 ms.

## B. Bugs

### B1. An analysis beside another of its kind renames every vector

Adding an `.AC` to the RC example, which has a `.TR` and an `.FFT` (the FFT writes an "ac"
plot too), and simulating (`p14`): the dataset's names all get their analysis's name.
`tran.v(out)` becomes `tr1.tran.v(out)`, the FFT's `ac.s` becomes `fft1.ac.s`, and the
new run's `ac.v(out)` becomes `ac1.ac.v(out)`. The example's own two diagrams, `tran.v(in)`,
`tran.v(out)` and `ac.s`, go blank ("has no variable tran.v(in); its variables:
tr1.tran.v(in), ..."). `get_dataset {variables: ["tran.v(out)"]}` and `["v(out)"]` are
refused, though `tran.v(out)` is the only transient vector of that name and the tool's
description offers `v(out)` as "each analysis's". It bit this hunt's own probes twice (p1,
p26).

**Fix**: resolve a name without the analysis's prefix when one analysis has it (as the
other spelling of `503e67f` resolves `v(...)`). Or prefix only the analyses whose kind is
shared, and only their vectors.

**Fixed in `04e762a`.** Only the simulations of a kind another writes too have their
vectors prefixed (`a_prefixedSims`): with an .AC beside the .FFT, `tran.v(out)` stays so, and
`ac1.ac.v(out)` and `fft1.ac.s` are prefixed. A name without the prefix is found when one
vector has it - by both graph readers, text and binary, and `get_dataset` - so the FFT's
`ac.s` diagram draws. p14: the example's diagrams draw after the .AC is added, and
`get_dataset` answers `tran.v(out)` and `v(out)`.

### B2. A delta marker keeps its Δ lines after its reference is deleted

Markers 1, 2 (relative to 1), 3 (relative to 2), then `delete_marker 2` (`p4`). Marker 3
(now 2) loses its `delta` and `relative to`, but its text still reads `Δx: 2.000m, Δy:
-0.371, 1/Δx: 499.999` until something rebuilds it. Deleting the reference's *trace*
instead updates the text (`p29`).

**Fixed in `04e762a`** (`Diagram::forgetMarker`, by `delete_marker` and the window's
Delete): the markers measuring from a deleted one lose it, and their Δ lines, at once.

### B3. Probe names an unnamed net as `get_schematic` names another

`set_label` refuses `net1`: "a name get_schematic gives a net without a label: another net
may be called so now or later". Probing an unnamed net (`p28`) labels it `net1` itself, and
`get_schematic` then lists two nets called `net1`: V1.1-R1.1 (unlabelled) and R4.2-R5.1
(labelled by the probe). The netlist keeps them apart (`_net0`, `net1`), but a tool given
`net1` now names either.

**Fix**: label probed nets from a name `set_label` would take (`probe1`, ... not taken).

**Fixed in `04e762a`:** a probed net is labelled `probe1`, `probe2`, ... ; p28's
`get_schematic` lists one net1.

### B4. Smith circles outside the sweep are drawn at its end

`smith_circles {frequency: 1e15}` on the S-parameter template's 1-20 MHz run (`p11`) answers
with circles at 20 MHz (`"at": 20000000`). `frequency: 1` gives 1 MHz, the sweep's start,
with nothing said either way. `get_dataset {measure: [stability], at: [1e15]}` refuses the
same frequency: "outside the sweep".

**Fixed in `04e762a`:** a frequency outside the sweep gives no circles, and says so: "1e+15
Hz is outside the sweep (1e+06 to 2e+07 Hz)".

### B5. `add_diagram`'s trace objects refuse `run`

`add_diagram {traces: ["tran.v(out)", {"variable": "tran.v(out)", "run": "before"}]}`:
"traces[1] has no run; it takes auto_color, axis, color, ghost, ..." (`p12`). Its
description says "objects as add_trace takes them", and `add_trace` takes `run`. A
before-and-after diagram therefore needs two calls.

**Fixed in `04e762a`:** `add_diagram`'s trace objects take `run`, drawn as a ghost unless
`ghost` is false.

### B6. `probe` of a trace already shown says it has no data

`probe {what: "in"}`, and a click on the `out` wire (`p8`): `"already there": true, "has
data": false, "text": "... Simulate to see it."`, while the trace it returns shows
`"points": 1425`.

**Fixed in `04e762a`:** a trace shown already, with points, has data - `"has data": true`,
and no "Simulate to see it".

### B7. Types of one domain take another's data without a word

`p26`:
- **Spectrogram of an AC sweep**: cuts the frequency axis as time ("segment": 62499 s,
  "loudest" at "time" 31259).
- **Bode, Nichols, pole-zero map of a transient**: accepted. The pole-zero map lists its
  1425 values as poles ("stable": false, "zeta": -1 for each positive one).
- **Probe into a pole-zero map, a constellation, a Smith chart or a bathtub**: adds a
  transient voltage with no remark (`p8`).

A note when the x is not the type's (time, frequency, a pole-zero run) would catch these.

**Fixed in `04e762a`.** A trace over another x than its type reads is named in the answers
of `add_diagram`, `edit_diagram`, `add_trace` and `probe`: "trace 1 (tran.v(out)) is over time:
a bode reads a frequency response (over frequency), its numbers mean nothing here"; a pole-zero
map's over time or frequency, "its numbers are no roots". A spectrogram of a run over frequency
is refused ("is over frequency (an AC run's): a spectrogram cuts a transient, over time").
`probe` into a pole-zero map, a contour map, a tornado chart, a truth table, a timing diagram
or a locus curve is refused; into a constellation it says the trace waits for its pair.

### B8. Colour Wires by Net: its rail names cannot name a net

The rule (`erc.cpp:1451`) and the action's What's This name +5V, -12V and 3V3 as supplies,
and `supplySign` names V+. A net label cannot be any of them: `set_label` refuses each, "a
letter first, then letters, digits and single _" (`p22`). The probe of 34 names
(`p21`):
- **Missed supplies**: `VDD_IO` and `VPP`.
- **Signals taken for supplies**: `VIN` and `VREF` (the rule lists `vin`, `vref`). Both
  are often a signal input and a reference.
- **Ground names**: `GND`, `AGND` and `GNDA` as labels are not drawn as ground.

**Fixed in `04e762a`.** The rails as a label can spell them are supplies: VCC and VDD with a
rail's suffix (VDD_IO, VCC_3V3, VDDA, VCCIO), VPP, P5V (a +5 V rail), N12V, V3V3, VBAT. VIN and
VREF are no longer (a signal and a reference as often), nor VCC_EN or VDD_OK. AGND, DGND, PGND
and GNDA as labels are drawn as ground. The What's This names them. p21: VDD_IO and VPP
supplies, VIN, VREF and VCC_EN signals, AGND and GNDA ground.

### B9. A marker moves when saved

A marker's x is written to 6 significant digits (`<Mkr 0.00400001 ...>`). After save and
reopen, or an undo (which reloads), it sits at another sample: 3.000006 ms became 3.000009
ms (`p4`). On the square's edge at 3 ms its reading flipped from `tran.v(in): 0.000` to
`1.000`. The marker line is upstream's. Delta markers make it more visible, since their Δx
changes too (2.000004 ms → 2.000006 ms).

**Fixed in `04e762a`:** a marker's x is written with as many digits as read it back as it
was (`QLocale::FloatingPointShortest`).

### B10. A stacked diagram puts seconds and hertz on one x axis

A transient in pane 1 and the FFT's spectrum in pane 2 (`p41`) are accepted on the one
shared x axis. The axis is labelled "time / frequency" and runs from 0 to 1 MHz, so the
transient is squeezed into a line at 0. A marker at 3 ms on the transient reads the
other pane as well: `ac.v(out): 0.316568`. That is the spectrum interpolated at 0.003 Hz,
written beside the transient's value as if it meant something. The panes share the x axis
by design, so a trace whose x is another variable than the first trace's should be refused,
or at least named in a note.

**Fixed in `04e762a`:** a stacked diagram with traces over different x says so ("Its traces
are over time and frequency: its panes share one x axis ..."), and its marker reads only the
traces over its own x - in its own notation and precision (N12).

### B11. A square wave is drawn falling before its edge

The RC's source, `tran.v(in)`, is exactly 1.0 at every sample up to its edge at 0.5 ms. It
falls through 0.9, 0.7 and 0.35 within the next 1 ns, and is 0 after (the dataset, `seeds/`).
Drawn over its 10 ms run (`files/a2_orphaned_limit_pass_b11_shoulder.png`, the lower pane),
each falling edge has a shoulder: a straight slope from 1.0 at about 0.3 ms down to 0.9 at
0.5 ms, then the drop. Over 1 ms (`p48`, `files/b11_edge_1ms.png`) the slope is shorter,
from about 0.49 ms. The samples near the last one drawn seem to be skipped regardless of
their y, so the line joins a sample well before the edge to the 0.9 inside it. The curve
then shows the edge starting 200 µs (2 % of the span) early, a falling ramp that is not in
the data. Its cause in the drawing code, and when it began, were not found in this hunt.

**Fixed in `04e762a`.** In `Graph::drawLines` the last point passed over, as nearer than a
pixel to the last drawn, is drawn to before the next that is not, so a corner stays where it
is. And lines thinned for a small view are thinned again when drawn larger: the hunt's exports
came after a view drawn small, and kept its coarse lines. `b11_edge_1ms.png` drawn again: the
edge is square.

### B12. Probing a transistor's emitter makes every run fail

On the shipped `2N3904_follower.sch` (`p53` to `p55`), `probe Q2N3904_1.1` and `.2` add
`@q2n3904_1[ib]` and `[ic]`, and the run succeeds with them. `probe Q2N3904_1.3`, the
emitter, adds `@q2n3904_1[ie]` to each `write` line. ngspice 46 does not produce that
vector, and the simulation fails: "Error during 'write': no writable vector found". That
is not one empty trace: the whole run, the example's own diagrams included, has no
results until the trace is deleted. In the window it takes one click in Probe mode.

**Fix**: give the emitter's current as -(ib + ic), an equation the probe adds as it does for
expressions; and check each device's current names against what ngspice saves (`p53`'s
list of the dataset's `@` vectors).

**Fixed in `04e762a`** with A5: the emitter's current is `-(@q1[ic] + @q1[ib])`, computed by a
NutmegEq of the vectors ngspice does write. p55: the run with the emitter probed succeeds, its
diagrams with it.

### B13. Under Qucsator, probing a net says the netlist cannot be written

A divider simulated with Qucsator (`p57`; `set_simulator qucsator`, the run succeeds). Then
`probe {what: "out"}`, a labelled net, answers "The netlist could not be written (Check
Schematic says why)". Check Schematic finds 0 errors, 0 warnings and 0 notes.
`get_netlist` writes the Qucsator netlist, and the next run succeeds. So under Qucsator a
net's voltage cannot be probed at all, and the message sends the user looking for a fault
that is not there. (A pin's current and a part's power are refused with the right reason:
"Qucsator saves no device's current or power".)

**Fixed in `04e762a`:** under Qucsator the nets' names are read from Qucsator's own netlister
(`prepareNetlist`, the DC bias shown kept). p57: `probe out` adds out.Vt.

### B14. The LTspice import drops `.step`, `.meas`, `.four` and `.wave` without a word

An `.asc` whose resistor is `{Rv}` (`p59`), with these directives:
- `!.param Rv=1k` and `!.tran 5m`;
- `!.step param Rv 1k 3k 1k`;
- `!.meas tran vmax MAX V(out)`, `!.four 1k V(out)`, `!.backanno` and `!.wave ...`.

The import answers with two notes, the parts and the `.tran` conversion. The netlist keeps
`.PARAM Rv = 1k` and nothing else of these: the `.step` sweep (three runs in LTspice)
becomes one run at 1k, and the `.meas` and `.four` results are gone. The run succeeds, so
nothing shows the difference. The round's write-up says what is left out is said, as it is
for `Rser`. `.step param` could become a parameter sweep, and `.meas` ngspice's own `meas`;
at least each should be named in the notes.

**Fixed in `04e762a`.** `.step param X start stop step` (and `dec`, `oct`) becomes a
Parameter Sweep of the first analysis, and `.four` a Fourier analysis of the transient;
`import_netlist`'s "converted" names each. A `.step` of a list, a source or the temperature
is said, not taken. `.meas` and `.wave` are named in the notes with what does it here. p59:
SW1 over Rv, 3 points, and FOUR1.

### B15. An LTspice capacitor with its ratings imports but never simulates

A capacitor as LTspice's component database writes it: `SYMATTR Value 1u`, `SYMATTR
SpiceLine V=50 Irms=1 Rser=0.01`, `SYMATTR SpiceLine2 Lser=1n` (`p60`). The import's note
says "C1: V=50 Irms=1 Rser=0.01 left out (LTspice's own, not ngspice's)", but the netlist
line is `C1 g 0 1u V=50 Irms=1`. Only `Rser` was left out, and `Lser` went without a note.
ngspice stops at it: "unknown parameter (v)", "no simulations run!". Every capacitor picked
from LTspice's database carries such ratings, so a real-world `.asc` often imports cleanly
and then does not simulate. The note claims the opposite of what the netlist holds. (A
MOSFET's `SpiceLine W=10u L=1u m=4` is kept, rightly; a source's `Value2 AC 1` is kept.)

Inductors are the same (`p61`). `SpiceLine Ipk=1 Rser=0.05 Rpar=1000 Cpar=1p mfg="Coilcraft"
pn="XAL4020" type="ferrite"` is all "left out" by the note, but the line is `L1 g 0 1u
ipk=1 mfg="Coilcraft" pn="XAL4020" type="ferrite"`, and ngspice stops at "unknown parameter
(ipk)". Leaving out `Rser` matters on its own: in LTspice an inductor's series resistance
keeps a source and inductor in parallel solvable. Here Check Schematic rightly finds V1
and L1 in parallel with no operating point, a fault the LTspice schematic did not have.

**Fix**: drop the ratings the note names (V, Irms, Ipk, Rpar, Cpar, mfg, type, ...) from the
SPICE line, and turn `Rser`, `Lser` into the series parts they are (or say so).

**Fixed in `04e762a`.** A resistor's, capacitor's and inductor's parameters - in its value,
its SpiceLine and SpiceLine2 - that ngspice does not take are left out, and the note names
exactly those: "C1: V=50 Irms=1 left out". Rser, Lser, Rpar and Cpar (a source's Rser and
Cpar) become parts of their own, in series or across as LTspice draws them: "C1: lser=1n
rser=0.01 made parts of their own (LC1_lser, RC1_rser)"; one of 0 is none. p60 and p61
simulate.

### B16. A Nichols chart's grid grows with its range: 105 MB for one diagram

The Nichols chart of the FFT's spectrum (`p71`, `p72`), whose nulls reach far below 0 dB,
exports as a 105 MB SVG and a 37 MB PDF. The whole schematic's PDF is 40.7 MB. With
`grid: false` the same chart is 140 KB, and the grid alone, with no trace, 663 KB. The M and
N contours are drawn over the whole axis range the trace sets, apparently at a fixed step
in dB, so a trace down to some hundreds of dB multiplies them. A print or a PDF of such a
schematic is that large, and so is the painting. On an ordinary loop gain (the RLC's, +20 to about -100 dB,
`p75`) the chart is a 670 KB SVG and a 304 KB PDF, so it takes a deep null or a wide range.

**Fix**: draw the contours only over the range where they are read (a closed loop from
about -40 to +40 dB, as Nichols charts do), however far the axis goes.

**Fixed in `04e762a`.** The cause was the phase, not the dB: the FFT's spectrum unwraps to
many turns, and the contours were drawn for each. They are now drawn only in sight (a point
either side of what is), and not at all when the phase spans more than 8 turns, too dense to
read - `get_schematic` says so ("grid not drawn"). p72: 105 MB → 170 KB, the grid alone 663 KB
→ 26 KB; p75's loop gain 670 KB → 174 KB.

## N. Minor

- **N1** `probe {what: ["R1", "C1", "out"]}` (a list of names) is read as a place:
  "nothing to probe at 0, 0". **Fixed in `04e762a`:** a list of names is probed name by name (`"probed": [...]`).
- **N2** A spectrum whose `from` is past the run's end answers without an `analyses`
  entry: the diagram is empty with no reason given. **Fixed in `04e762a`:** the trace's `analyses` entry says why ("'from' (0.02) is at or past the run's end (0.01)").
- **N3** A tornado chart's `at` outside the sweep (99 s of a 10 ms run) uses the last
  point silently. A bar of value 0 is counted "of nothing", with the bars that have no
  data. **Fixed in `04e762a`:** `"at outside": "99 is beyond the sweep (0 to 0.01): the values at its nearest end are shown"`; the bars of 0 are `"of 0, not shown"`, apart from those `"without data"`.
- **N4** A constellation's `symbol_period: 1e-15` stops at 20,000 symbols (the first 20 ps)
  and says only `"symbols": 20000`. `get_dataset evm` does the same. **Fixed in `04e762a`:** the pair, and `get_dataset evm`, say "the first 20000 symbols only, to 1.99995e-11 of the run's 0.01 (at most 20000 are taken: ...)".
- **N5** `add_painting {type: bus, name: ""}` is accepted while every other malformed name
  is refused. Check Schematic warns about it afterwards. **Fixed in `04e762a`:** refused, with the form a bus's name takes.
- **N6** A limit of three points at one x (`[[0,1],[0,1],[0,1]]`) is accepted, and passes. **Fixed in `04e762a`:** refused, "its points are all at x = 0: they span no x".
- **N7** In a saved file, a limit with a negative pane, or points that do not parse, fails
  the whole schematic's load ("Wrong 'diagram' line format"). A trace pane of 99 or -7, or
  a limit pane of 50, loads, clamped or orphaned (A2). **Fixed in `04e762a`:** a limit line that does not read is left out and the schematic read; a negative pane is the first, one past the panes the last (A2).
- **N8** An LTspice symbol's `SpiceOrder` of 0, -1, text, or one number twice, is taken
  without a warning, and the pins are put in some order (`p31`). **Fixed in `04e762a`:** "mysub.asy: SpiceOrder 0 (the pin at 0, 0) is not 1, 2, 3 ... each once - those pins put last, in the order drawn: check its nodes in the netlist".
- **N9** A box plot's `at` on a trace of one curve is ignored (all its values are taken),
  but echoed back as if used. **Fixed in `04e762a`:** the box says `"at": "not used: the trace is one curve, all of whose values are taken ..."`.
- **N10** A contour's `range` entirely below the data (0 to 1e-300) lists nine iso-lines,
  1e-301 to 9e-301, none of which can be drawn. **Fixed in `04e762a`:** the iso-lines listed are those the data reaches; `"iso-lines not drawn": "9 of the range's beyond the data (1 to 20): no line"`.
- **N11** Diagram Properties, opened and closed with OK, writes 1e-14 back as
  1.0000000000000002e-14 (the bathtub's floor), and 1e-4 as 9.999999999999999e-05 (the
  constellation's offset) (`p23`). **Fixed in `04e762a`:** a field read back as it was shown keeps the value it was shown with (`keptEdit`, `readKept`), and a prefix below one divides by its power of ten (10f is 1e-14 exactly); p23 changes nothing.
- **N12** A stacked diagram's marker reads its own trace at its precision and the other
  panes' at six digits: `tran.v(in): 0.000` above `tran.v(out): 0.358744`. **Fixed with B10 in `04e762a`:** the other panes in the marker's own notation and precision.
- **N13** `add_marker {relative_to: 2}` on a diagram with no marker says "1 to 1". A
  bathtub's `levels: 4` on an NRZ signal draws three eyes without a remark. **Fixed in `04e762a`:** "'relative_to' is the number of another marker of this diagram, and it has none yet"; four levels whose middle two hold under a tenth of the symbols are said, "is it NRZ? (levels 2 reads it so)", with the bathtub's and the eye's notes in `get_schematic`.
- **N14** The LTspice import (`p42`):
  - a UTF-16 big-endian file is not taken for an `.asc`, even with its BOM, and is
    parsed as a SPICE netlist ("No elements were found");
  - a file with old-Mac CR line ends has "no parts";
  - lines starting with a NUL byte are dropped without a note (two wires lost, 14 nets for
    11).

  LTspice writes UTF-16 LE or 8-bit, so these are rare. **Fixed in `04e762a`:** UTF-16 big-endian, with its mark or without, and CR line ends are read; a line of none of an `.asc`'s words is said ("1 line(s) not read ...: line 9 (WIRE 0 64 100 64)").
- **N15** Insert > Bus with both clicks on one point keeps a bus of no length and no name
  (`p44`). **Fixed in `04e762a`:** a second click on the first point draws on, and a bus drawn without a name says in the status bar how to name it.
- **N16** `check_schematic` warns once per stretch beyond a limit (`p49`). An upper limit
  of 0.9 V on the 10-cycle square gives ten warnings, one per cycle, where one per limit
  (with the count) would do. **Fixed in `04e762a`:** one warning a trace and limit, the worst stretch, "(the worst of 10 stretches beyond it)".
- **N17** A tornado chart's `at` takes the sample nearest to it (3,070 Ω for 3,000 on the divider's sweep: 0.031546, not 0.032371) and answers `"at": 3000`, while a box plot's `at` interpolates (`p51`). **Fixed in `04e762a`:** straight between the samples, as a box plot's (0.032371).
- **N18** An LTspice part named after a model of LTspice's own library (a diode `1N4148`, from its `standard.dio`) imports with no note (`p62`); Check Schematic finds nothing, and the run fails: "could not find a valid modelname". The round's write-up names un-followed `.lib` files as a limit; a model named only in a part's value is not mentioned at all. **Fixed in `04e762a`:** "D1 (1N4148): models defined nowhere in it - LTspice takes them from its own library (standard.dio, standard.bjt, standard.mos ...), which ngspice does not have: add their .model (or .lib) lines, or the run fails" (with `.lib` or `.inc` lines, that the files must define them).
- **N19** LTspice flags whose names a Qucs-S label cannot take are renamed without a note (`p69`): `+5V` becomes `_5V`, `-5V` `_5V_2`, `V+` `V_`, `V-` `V__2`, `a.b` `a_b_2`. The nets stay distinct, but nothing says which is which. And `5V` and `3V3` are kept as labels that the label dialog and `set_label` refuse (a letter first). **Fixed in `04e762a`:** the flags are given names a label takes, said: +5V P5V, -5V N5V, 5V V5V, V+ VP, V- VN, 3V3 V3V3, a.b a_b_2; a SPICE netlist's nodes too (`import_netlist`'s "converted").
- **N20** The pole-zero map lists zeros with a pole's `"stable"` (`p73`): an RC high-pass's zero at the origin is `"stable": false`. For a zero the property is minimum phase (left half-plane), and a zero at 0 is neither stable nor unstable. **Fixed in `04e762a`:** a zero has `"minimum phase"`, a pole `"stable"`; one on the axis `"on the imaginary axis": true`, as the readout says.
- **N21** A constellation (and `get_dataset evm`) whose offset is past the run (1 s of 10 ms), or whose symbol period is longer than the run, answers "no sample from 0 on", naming `from` rather than the offset or period at fault; a negative offset is taken without a word (`p74`). **Fixed in `04e762a`:** "the offset (1) is past the run's end (0.01)", "a symbol period of 1 is longer than the run from 0 to 0.01", "'from' (...) is past the run's end"; a negative offset is refused.
- **N22** Values at the Marker is not kept with the schematic (`p76`): after a save and reopen the labels are gone and `get_schematic` has no `values at the marker`, though the marker (`annotate: true` when placed) is still there. **Fixed in `04e762a`:** kept in the file (`<ValuesAtMarker=n>`, the marker it follows by its number), and shown again on opening.
- **N23** A delta marker on a complex trace (a Smith chart, a polar diagram, `p77`) gives as "Δy" the difference of the magnitudes: |Γ₂| - |Γ₁| = 0.2273 - 0.1390 = 0.0883 for S11 at 15 and 5 MHz. On those charts neither axis is the magnitude; the distance |Γ₂ - Γ₁| is 0.1075. It should say Δ|Γ|, or give both. **Fixed in `04e762a`:** in the complex plane (Smith, polar) the delta gives `"magnitude"` (Δ|y|) and `"distance"` (|Δy|), and the label both.
- **N24** The round's write-up (`docs/feature_gaps/2026-10-07-claude-missing-diagrams.md`) says "`tune`'s `hold` takes `{limits: n}`"; the tool refuses that ("hold[0] has no limits; it takes max, measure, min") and wants `{measure: {limits: n}, max: 0}` (`p81`). **Fixed in `04e762a`:** the write-up says `{measure: {limits: n}, max: 0}`, and that a limit no trace is drawn against is refused.

## The fixes, checked

Where:
- `diagrams/diagram.cpp` and `diagrams/marker.cpp`: the log axes and the marker (F1), a vector's
  simulation's prefix (B1), a deleted reference (B2), a limit that does not read (N7);
- `diagrams/spectrogramdiagram.cpp` (F2, B7); `ltspiceimport.cpp` (F3, B14, B15, N8, N14, N18,
  N19); `diagrams/tornadodiagram.cpp` (A1, N3, N17); `diagrams/stackeddiagram.cpp` and
  `diagrams/speclimits.cpp` (A2, B10, N12); `spectrum.cpp` (A3); `eyeanalysis.cpp` (A4, N13);
- `probe.cpp`: a pin's current, computed where ngspice writes none (A5, B12), Qucsator's
  netlister (B13), the names (B3), a trace shown (B6);
- `extsimkernels/ngspice.cpp` and `abstractspicekernel.cpp` (B1); `dataset.cpp` and
  `diagrams/graph.cpp` (B1, A5's spelling; B11's corner and scale);
- `diagrams/smithdiagram.cpp` (B4), `diagrams/nicholsdiagram.cpp` (B16),
  `diagrams/constellationdiagram.cpp` (N4, N21), `diagrams/polezerodiagram.cpp` (N20);
- `erc.cpp` (A2, B8, N16), `qucs_init.cpp` (B8's What's This);
- `paintings/buspainting.cpp`, `mouseactions.cpp` and `qucscontrol_paintings.cpp` (N5, N15);
  `valuereading.cpp` and `diagrams/diagramdialog.cpp` (N11); `schematic_file.cpp` (N22);
- the tools: `qucscontrol_results.cpp` (`add_diagram`, `edit_diagram`, `add_trace`, `add_marker`,
  `probe`, `get_schematic`: F1, A1-A6, B1, B4-B7, B10, B16, N1-N4, N6, N9, N10, N13, N20, N21,
  N23), `qucscontrol_design.cpp` (`import_netlist`: B14, N19; `tune`'s hold: A2);
- the round's write-up (N24).

Tests:
- `test_diagram_tools`, eleven cases, each finding's case as it was found:
  `huntMarkerOnALogAxisFromZero` (F1), `huntASpectrogramsShortSegment` (F2, B7),
  `huntLtspiceImport` (F3, B14, B15, N8, N14, N18, N19), `huntTornadoParts` (A1, N3, N17),
  `huntSpectrumHarmonics` (A3, N2), `huntProbedCurrents` (A5, B12, B13, N1), `huntMarkers` (A6,
  B2, B9, N13, N23), `huntLimits` (A2, N6, N7, N16), `huntPrefixedNames` (B1, B6),
  `huntTheRest` (B4, B5, B8, B16, N4, N9, N10, N13, N20-N22) and `huntBusesAndDialogValues`
  (N5, N11, N15). With ngspice they simulate: the probed currents against the device's (the
  sign, the emitter's sum), an .AC beside the example's .FFT, the PRBS example read as PAM4,
  the stepped LTspice import. `probingAnUnnamedNet` (B3) and `ltspiceImport` (a source's Rser
  is a part now) changed with them.
- `test_stacked_diagram`: `aLogAxisFromZero` (F1, which it asserted the other way:
  nothing drawn), `aLimitGoesWithItsPane` (A2), `aSquaresEdgeIsWhereItIs` (B11),
  `aMarkerReadsEveryPane` (B10, N12).
- `test_prbs_eye`: the width at 1e-12 is the bathtub's opening (A4); `test_binary_dataset`:
  a prefixed name read from a binary dataset as from text (B1); `test_qucs_control`: a SPICE
  netlist's `.four` taken (B14).

Each fix was broken on purpose and the tests run: 74 breaks, all caught. Eight were not caught
at first, and their tests were made stronger: A3's peak (a square sampled as ngspice samples
it, where the floor has ripples a bin away), B1's name resolution, B6 (a trace whose data the
run names otherwise), B9 (samples a nanosecond apart), both halves of B11 (the lines a graph was
drawn with), B16 (the chart's size with its grid and without), N11 (QCOMPARE of doubles is
fuzzy; and a dialog's round trip). Writing those tests found two more things, fixed before the
commit:
- the emitter's computed vector came out as the name alone (ngspice gives a sum of untyped
  vectors no type, where `-@r1[i]` stays a current): both readers take a computed current
  either way;
- the first `add_trace` that kept a trace's own spelling took an analysis's prefix for a
  simulation's (`v(out)` lost its `ac.`): the full suite caught it.

Then:
- the full suite 95/95; under AddressSanitizer and UBSan 95/95, no report (a parallel run had
  `test_drop_open` wait 5.55 s for 5 and a tune of `test_qucs_control` fail under load; both
  pass alone);
- the end-to-end scenarios 96/96 (s7, which copies a project of the user's workspace,
  skipped);
- the hunt's probes again, each answering as its fix says: p3, p5, p8, p11, p12, p14, p16, p18,
  p19, p20, p21, p23, p24, p28, p31, p34, p36, p38, p39, p42, p48, p51, p52, p55, p57, p59-p62,
  p65, p69, p72-p75, p77-p79, p81;
- the fuzzers on the ASan app: `fuzz-sch.py` seed 11, 400 mutants of the seed (800 runs), no
  failure; the LTspice fuzzer (p9, seeds 1-3) 2,250 mutants, no crash and no report (F3's three
  crashes before).

CI's Linux runner, a stock ngspice-42, then failed two cases that pass on this machine's
ngspice (a build of its own). **Fixed in `286d644`.**
- A stock ngspice writes a device's current (`@r1[i]`, `@q1[ic]`) as `i(@r1[i])`, where the one
  here keeps the name: a probed part's trace (`tran.@r1[i]`) found no data, and
  `huntProbedCurrents` read 2 of its 5 vectors. Qucs-S now reads such a vector under the one
  name, and a graph takes either spelling for a dataset written before. Run against a stand-in
  ngspice that writes the stock names, the case fails without the fix and passes with it.
- A stock ngspice has no PRBS source: `huntTheRest` leaves N13's run of the PRBS example out
  there, and says so.

## What was found right

- **The pole-zero map** of a real `.PZ` (an RLC, `p15`): poles -11,270.2 and -88,729.8
  rad/s, as by hand; f, ζ and Q follow from them. With zeros (an RC high-pass, `p73`): the pole at -1000 rad/s and the zero at the origin, each under its kind.
- **The Bode margins**: the phase margin of 10·v(out)/v(in) is 55.087° at 13,052 Hz,
  against 55.084° at 13,050 Hz by hand. No phase crossover (-179.09° at 1 MHz) is reported
  as none.
- **Two gain crossovers** (`p40`): for T = 0.5·H of an RLC of Q 10.5, which crosses 0 dB
  at 3,575 Hz (margin 172°) and at 6,136 Hz (13.4°), the phase margin given is the worse,
  13.376° at 6,136 Hz, as by hand. It does not mention the other crossover.
- **A million samples** (`p43`, a `.npy` imported): the spectrum, spectrogram, bathtub, box
  plot, constellation, tornado, stacked and eye diagrams each add and export in 0.2 s or
  less, and the schematic saves, closes and reopens in 1.2 s.
- **Markers on every new type** (`p68`, ASan): stacked, Nichols, pole-zero, polar and Smith take one and move it; bathtub, contour, spectrogram, tornado, box plot, constellation and spectrum refuse one with the reason. Only the Bode diagram over data from 0 Hz crashes (F1).
- **Check Schematic's bus rules** (`p70`): a wire ending on `D[7:0]` labelled `D9`, and one unlabelled, are flagged with the reason; one labelled `D3` is not.
- **SVG and PDF exports** of every new type (`p71`, ASan): no report; all but the Nichols chart (B16) 4 KB to 200 KB.
- **Stability without S-parameters** (`p80`): `get_dataset stability` of a node voltage and Smith circles on a run with no S-parameters each refuse with what they need.
- **Probing a subcircuit** (`p66`, the quartz example): its pins and the instance are refused with the way to do it (a net of it, or an iProbe), and the run is untouched. A MOSFET's pins give `ig`, `id` and `is`, and the runs succeed (`p56`).
- **The window's Probe and Bus modes** through clicks (`p44`, ASan): wires, pins, parts,
  empty canvas, a diagram and a simulation block clicked, a right click, Escape. There
  was no report, and the parts' powers were added as traces.
- **The box plot of a swept trace** (`p50`): each of 11 curves' values at R1 = 5,050 Ω and at 3,000 Ω (between grid points, interpolated), quartiles, mean, min and max, equal numpy's.
- **The contour map** of a divider swept over R1 and R2 (`p16`): the passing share for
  `min: 0.4` is 0.6667, the grid's 154 of 231 points by hand. `pass` and `range`
  refuse min ≥ max, and `levels` refuses more than 50.
- **The box plot** (`p17`): q1, median, q3, mean, the Tukey whiskers and the 36 outliers
  equal numpy's (type 7) on the dataset's 1,425 values.
- **The constellation's EVM** (QPSK, 20 symbols): 62.846 % rms and 78.714 % peak, as by
  hand.
- **The THD** of the square, 42.90 %, against 42.88 % by hand (harmonics 2 to 9). The
  spectrum's and `thd`'s agree to 0.03 %.
- **Values at the Marker under Qucsator** (`p58`): a divider's `out.Vt` labelled 0.5 V and `top` 1 V at the marker.
- **Values at the Marker on an AC sweep** (`p30`): out 0.31755 at -89.85°, mid 0.00265 at
  -89.85°, as by hand. The values follow the marker when it moves, go when it is deleted,
  and come back with undo.
- **The Smith circles** (`p11`): available gain circles of 1000 dB converge to the input
  stability circle, as they must when K < 1. Frequencies of 0, below 0, "abc" and "nan" are
  refused; an unknown kind is refused; the chart types without circles are refused.
- **The LTspice import's rotations**: a res, a cap and a voltage source at R0, R90, R180 and
  R270 each join the two flags where LTspice puts their pins (`p25`).
- **Delta markers** in a loop (1→3→2→1) move without recursion, and each Δ is right. A delta at its reference's own x leaves 1/Δx out rather than dividing by zero (`p63`). A
  delta to itself and to a marker that is not there are refused.
- **Panes**: 0, 9, -1 and [] are refused. A trace's pane past the count is refused, and on
  a reduction a trace's pane is clamped. Undo and redo of a reduction restore the panes.
- **The Diagram Properties dialog** keeps every new type's settings through OK (apart from
  N11).
- **Log axes over 0 and below**: every new type exported without a sanitizer report
  (`p27`).
- **Tiny sizes**: the new types at 1×1 to 30×20 drew without a report, until F1's marker
  (`p32`).
- **The overlay**: a kept run's ghost, `overlay: null` removing it, an unknown run
  refused.
- **Buses**: every malformed name (`D[a:b]`, `D[7:0`, `[3:0]`, `D[100000:0]`, a list)
  refused with the form it takes.
- **Fuzzing**: `fuzz-sch.py` (seeds 8, 9, 10) found no failure in 1,000 mutants of the seed
  (600 and 800 runs, load+netlist and render). The LTspice fuzzer found only F3.

## How to run it again

The probes need the two builds and ngspice. Each takes `QUCS` (the app's binary) and
`HUNT_RUNS` (a scratch folder for the servers' workspaces, settings, HOME, trash and
cache):

```bash
export HUNT_RUNS=/tmp/qucs-hunt-runs QUCS="$PWD/build/qucs/qucs-s.app/Contents/MacOS/qucs-s"
python3 docs/bug_hunts/2026-10-08-new-diagrams/p34_bode_marker_crash.py "ac.v(out)" 400 360   # F1
python3 docs/bug_hunts/2026-10-08-new-diagrams/p3_spectrogram_hang.py                          # F2
python3 docs/bug_hunts/2026-10-08-new-diagrams/p19_tornado_underscore.py                       # A1
python3 docs/bug_hunts/2026-10-08-new-diagrams/p5_limit_orphan_loop.py                         # A2
python3 docs/bug_hunts/2026-10-08-new-diagrams/p24_thd.py                                      # A3
python3 docs/bug_hunts/2026-10-08-new-diagrams/p52_probe_sign.py                               # A5
python3 docs/bug_hunts/2026-10-08-new-diagrams/p65_delta_renumber.py                          # A6
python3 docs/bug_hunts/2026-10-08-new-diagrams/p55_bjt_each_pin.py                            # B12
python3 docs/bug_hunts/2026-10-08-new-diagrams/p57_probe_qucsator.py                          # B13
python3 docs/bug_hunts/2026-10-08-new-diagrams/p60_ltspice_spiceline.py                       # B15
```

F3 needs the ASan (Debug) build:
`QUCS=.../build-asan/... python3 p10_ltspice_repro.py files/qcheckedint_abort.asc`.
The LTspice fuzzer is `p9_ltspice_fuzz.py <seed> <count>`. The file edits are
`p7_file_fields.py seeds <out>`. The schematic fuzzer over the seed:

```bash
python3 scripts/ci/fuzz-sch.py build-asan/qucs/qucs-s.app/Contents/MacOS/qucs-s docs/bug_hunts/2026-10-08-new-diagrams/seeds out --count 300 --seed 9
```

(`seeds/rc.sch` names its dataset RC_filter_FFT.dat. To give the fuzzer the dataset, copy
`rc.sch` to `RC_filter_FFT.sch` beside it, as seed 9's run did.)

The fixes' breaks: `run_breaks.py [name ...]` undoes each fix of `breaks.py` in turn (all
when no name is given), rebuilds the test it names in `build/` and runs it - which must fail -
then puts the source back.
