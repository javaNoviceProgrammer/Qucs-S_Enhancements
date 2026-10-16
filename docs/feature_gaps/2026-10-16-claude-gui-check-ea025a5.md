# GUI check 2026-10-16 (8c1ad8a, 0231545, ea025a5): each item

The reviewer's GUI check (`2026-10-16-claude-gui-check-ea025a5.md`; its files in
`~/QucsWorkspace/update_check_1016c/`, copied to a scratch folder and read there, never changed)
found the expressions, the Markdown images and the maths right. It listed six items. Each was
reproduced first through the build's own tools, then fixed and tested.

## The items

| # | Feedback | Now |
|---|---|---|
| 1 | Short names fail on an exported run: `rcsw:v(out)/v(in)` and `get_dataset path=rcsw.dat` said "there is no variable v(out)"; on the run's own dataset they work | Reproduced. A dataset's names were taken as a SPICE simulator's only when its file was a `name.dat.ngspice`. A run exported or kept as a plain `name.dat` keeps ngspice's names (`ac.v(out)`), but was read with Qucsator's rules, so the analysis could not be left off. Now the names decide too: a variable named `analysis.v(...)` or `analysis.i(...)` makes them a SPICE simulator's. On `rcsw.dat`, `v(out)` is `ac.v(out)` and the node `out` is its voltage. A Qucsator dataset is read as before: `out` is `out.v`, and `v(out)` is not found. |
| 2 | An expression of the independent variables alone is refused (`frequency*ac1.r1`, an analytic model over a swept run) | Evaluated over the grid of their sweeps, in the order of the variable over them all (the one over fewest), else the dataset's: `db(1/(1+j*2*pi*frequency*ac1.r1*1e-9))` gives 4 curves of 41 points, as `ac.v(out)` does. One independent variable alone is still over itself. Found on the way: an expression of a variable over nothing (`<dep x>`) read a null pointer and crashed. It evaluates now, and putting the old read back crashed the test. |
| 3 | `screenshot diagram=1` cut an outside legend off: its frame and colour lines, none of its text | Reproduced. The picture was cut to the diagram's selection frame. Now it is cut to all the diagram draws (`paintedRect`, measured in the page's font): its legend beside it and its axes' numbers. |
| 4 | One `get_dataset` answer gave two values at one point: 0.7071433 in the curve, 0.7072083 in the table (1/√2 = 0.7071068) | Reproduced. The curve interpolated the real and imaginary parts straight in f; the table interpolated the magnitude straight in f. Both now take one value (`ds::sampledAt()`). On a sweep alike in log f (an AC run's), it is a cubic in log f through the two samples about the point, its slopes from their neighbours: the log magnitude and the unwrapped phase of a complex value, or a real value. On a linear sweep (time) it is straight in x, as before. The RC at its corner: 0.70710665 in both places. Over the sweep an RC's response is within 8e-5 (straight in f: 1.3e-2), a 4th-order Butterworth's within 7e-3 (1.4e-1). Five ways were compared before choosing (straight in f or log f, magnitude, log magnitude, cubic). |
| 5 | The x label is repeated once per trace (four times "frequency"), the last cut by the paper's edge | Each x variable is written once: in the axis' colour when several traces share it, in the trace's own colour when one trace alone has it (diagram 2's `number`). A locus diagram's `real(...)` is each trace's own, as before. Diagram 1 now has one line where it had four. |
| 6 | Maths: no equation numbers; a math block in a list item centred on the page; align not checkable | **Numbers:** none, as on GitHub, whose dialect the viewer renders. LaTeX would number them, so a numbered environment's tooltip (equation, align, gather, multline, eqnarray, not starred) says "Not numbered, as GitHub shows it". **In a list item or a quote** a display formula now sits under its text, 24 pixels in, not centred on the page: in the viewer and the chat alike (`qucs_s::math::placeDisplay()`). **align** checked with uneven left sides (`x &= 1` beside `a + b + c + d &= 2`, and two column pairs in `aligned`): the `=` signs line up, the left sides to the right of them, the right sides from them. A test now checks that an aligned pair is wider than either row set alone. |

The "not tested" items: the chat's paste, a PDF refused and an image of more than 2000 pixels
are in `imagesDroppedAreShownToClaude` (ea025a5). The dialog's red hint and typed `dataset:expr`
are in `anExpressionOverAnImportedDataset` (8c1ad8a).

## Checked

- test_dataset:
  - spiceNamesInAPlainDataset (new): an exported run's `v(out)`, `out` and `v(out)/v(in)`; an
    analytic model of frequency and R1; Qucsator's names unchanged; a variable over nothing.
  - valuesBetweenSamples (new): the RC sampled ten to a decade. At its corner it is within 1e-6
    of the exact value (phase within 1e-4 rad), and within 1e-4 at 2000 points across the
    sweep. Phases through ±180° are unwrapped both ways. A real value on a log sweep, a linear
    sweep, a sample's own value, and outside the samples.
  - expressionsOfTheSweeps: `frequency*Rs` over Z's grid; `Rs + 0*frequency` in Z's order
    whatever the expression's order; `a*b` in the order of the variable over them (b, a), not
    the dataset's; two sweeps no variable is over, in the dataset's order.
- test_binary_dataset aGraphOfAnExpression: `frequency*Rs/1e6` drawn over both sweeps (it was
  refused). The x label is written once for three traces, and with another x beside it each is
  written once.
- test_qucs_control anExportedRunReadsAsItsOwn (new): an exported RC run swept over R1.
  get_dataset's short names; the curve's and the table's value at 159154.9 Hz are equal and
  within 2e-6 of 1/√2. The run and its analytic model traced (4 curves of 41 points each). A
  diagram's picture with its legend outside is more than 15 % wider for its size than with no
  legend: it was the same.
- test_markdown_doc mathIsSharpAndFound: display maths centred on its own, set in under a list
  item and in a quote; the equation's tooltip; align's columns.

Thirteen changes broken on purpose. Twelve were caught at once:
- SPICE names by the file's suffix alone;
- the sweeps alone refused;
- the dataset's order always;
- straight in log x;
- the table straight in x;
- the other direction of the phase not unwrapped;
- no log sweep found;
- a diagram's picture cut to its frame;
- each graph's own x label;
- display maths in a list centred;
- no word of the numbers;
- the null read put back (the test crashed).

One was caught after its test was made to see it: **the phase not unwrapped from +π to -π**. The
test's phase crossed ±180° one way only. It now crosses both ways, and each of the two unwrapping
loops is needed.

The Release suite 126 of 126 (two tests changed with the behaviour, see above). The touched tests
under the sanitizers: no report.

## Not done, said

- **The canvas's paper cut below a diagram's labels:** with one label for four, the reviewer's
  case no longer runs past it. The paper's edge itself was not reproduced offscreen (the canvas
  there is a few hundred pixels).
- **Equation numbers** are not drawn (GitHub's way). `\tag` is not supported.
- **Interpolation inside a narrow peak** (a resonance of Q 10 sampled ten to a decade) stays
  rough by any rule: the samples do not hold it.
