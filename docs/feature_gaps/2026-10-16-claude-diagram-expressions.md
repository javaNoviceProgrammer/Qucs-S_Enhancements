# A diagram's graph of an expression: 2*L, 2*pi*frequency*L, v(out)/v(in)

The request (2026-10-16): "Looks like the "Graph input" in the 1D diagram doesn't accept an
arbitrary math expression based on all "indep" and "dep" loaded variables. Why is that? I want
to be able to write any arbitrary math expression and it should be automatically calculated."
The screenshot: Edit Diagram Properties, a graph "2*L" over the imported dataset
coax_rlgc_pg1d2 (frequency, independent, 9 points; L over frequency), drawn as nothing.

## Why it did not

Four things, each enough:

1. **The graph read its text as a variable's name.** Graph::loadDatFile looked for a block
   `<dep 2*L ...>` in the dataset; there is none, so the graph had no data. An expression
   evaluator was there (dataset.h's evaluate, get_dataset's and export_data's), but no diagram
   used it.
2. **That evaluator took dependent variables only.** frequency, time, a swept parameter were
   left out of the names it looked up: 2*pi*frequency*L was "there is no variable frequency".
3. **Typed text read the schematic's own dataset.** A variable double-clicked in the list gets
   the dataset chosen above put before it (coax_rlgc_pg1d2:L) and the simulator's prefix
   (ngspice/); a name typed in the Graph Input and added got neither, so 2*L was looked for in
   the schematic's dataset, not in the imported one the list was showing.
4. **A slash was always a simulator's prefix.** loadDatFile, the dialog's list of files and
   Claude's trace lookup took everything before the first "/" as the simulator: v(out)/v(in)
   was the variable "v(in)" in a file name.dat.v(out); sqrt(L/C) over coax_rlgc_pg1d2 the
   variable "C)" in coax_rlgc_pg1d2:sqrt(L... And the expression parser read a slash inside a
   name as part of it: L/C was one name, "there is no variable L/C". The labels cut the same
   way: a marker, a table's column, a stacked pane and the axis showed v(in) for v(out)/v(in).

## What it is now

**A graph's text that is an expression is evaluated** (Graph::loadExpression, before the text
and binary readers): the dataset read (text or binary alike), the expression evaluated at each
sample, the graph over the sweeps its variables are over - one curve per value of a second
sweep, as a variable's are -, the part shown (dB, phase, magnitude...) taken of each value. Its
x the sweeps' values, or the variable it is plotted against (`2*L@C`, the dialog's Plot Vs.). A
variable whose own name reads as an expression (Qucsator's net-1.V, a Nutmeg equation's) is
that variable, as before.

**The expressions** (dataset.h's evaluate, so get_dataset, export_data and the graphs alike):

- numbers; `pi`, `e`, `j` (√-1) where no variable has the name;
- every variable - dependent, and now **independent**: frequency, time, a swept parameter its
  value at each sample of the others, the samples running over the sweeps fastest first
  (Z*Rs over frequency and Rs: Rs the same for each run of frequency); an expression of one
  independent variable alone is over itself (frequency^2); of two alone it is refused, said
  ("frequency and Rs are both independent: ... with a variable over both"); one the others are
  not swept over too ("frequency is not a sweep of the others (they are over time)");
- + - * / ^ and parentheses, in complex numbers where the values are;
- the functions db, abs, mag, phase (degrees), real, imag, conj, sqrt, exp, ln, log10 as before,
  and now arg (radians), sq, log (base 10, Nutmeg's), log2, sin, cos, tan, asin, acos, atan,
  sinh, cosh, tanh, floor, ceil, round, sign - each the real function on a real value (bit for
  bit: log2 by the complex log was a last bit off), the complex one beyond (asin(3)).

A slash in a name is now only a simulator's prefix at its start (ngspice/tran.v(out) inside an
expression still reads); elsewhere it divides.

**The dialog**: a name typed and added takes the dataset chosen above and its simulator, as a
variable taken from the list does - 2*L over the imported coax_rlgc_pg1d2 is
`coax_rlgc_pg1d2:2*L` - unless it says its own (`other:...`, `ngspice/...`); a graph edited
keeps its own dataset (its text changed, not where it reads - the dataset chosen above may be
another's by then). An expression is shown evaluated: its tip "An expression of the dataset's
variables, evaluated at each of their samples"; one that does not evaluate is in red, its tip
why ("Not evaluated: there is no variable nosuch"), checked against the file the graph reads.

**The labels** keep an expression whole: the axis, a marker, a table's column, a stacked pane,
a spectrum's or a box plot's names (Graph::unprefixed: the simulator's prefix off, a division's
slash kept). One analysis shared by the diagram's graphs is left off each name in it:
`ngspice/db(ac.v(out)/ac.v(in))` is labelled db(v(out)/v(in)) - it was v(out)/ac.v(in)), the
name cut at its first dot.

**Claude's tools**: add_trace and add_diagram take an expression of an imported dataset's
variables (`dummy_data:2*pi*time*v1 + v2`): drawn at once. Under Qucsator and Xyce an
expression was refused ("only an equation block computes one"); it is now the diagram's too,
said in the note. Under ngspice an expression of the schematic's own run is still a NutmegEq's
variable, as before (it is then the dataset's, for every reader). A trace with no data says an
expression's reason ("there is no variable nosuch"), not "has no variable 2*nosuch". The
tools' texts say what an expression may hold.

## Checked

- test_dataset expressionsOfTheSweeps (new): 2*L; 2*pi*frequency*L against 2πfL (1e-15);
  1/(j*2*pi*frequency*C) - real part exactly nought, the imaginary -1/(2πfC); L/C a
  division; sqrt(L/C)*e/e; frequency^2 and Rs/2 over themselves; Z*Rs + frequency/1e6 over two
  sweeps, each sample; sixteen functions each bit for bit its real one; asin + acos π/2;
  sq, sign, arg; asin(2) complex; a variable named pi is the variable; the three refusals.
  expressionsAreEvaluated: ngspice/tran.v(out)/ngspice/tran.v(in), the prefix a name's.
- test_binary_dataset aGraphOfAnExpression (new): seven graphs from a text and a binary
  dataset alike - axes, x values and every value (1e-15): 2*ac.L, 2*pi*frequency*ac.L,
  ac.v(out)/ac.v(in) (complex), frequency^2, ac.Z*Rs (two sweeps, two curves), 2*ac.L@ac.a-b
  (against a dependent variable), ac.a-b (a variable so named); not read again unchanged; the
  phase of v(out)/v(in) that of v(out); three refused, each why, no data; coax:2*L and
  coax:sqrt(L/C) of another dataset (50 Ω); unprefixed, analysesOf, withoutAnalysis; the
  labels v(out)/v(in), db(v(out)/v(in)), 2*L and no v(in).
- test_data_import anExpressionOverAnImportedDataset (new) - the user's case through the
  dialog: a CSV imported (coax_rlgc), 2*L, 2*pi*frequency*L, sqrt(L/C), 2*nosuch (red, why),
  L (a variable: no tip) typed and added - each coax_rlgc:...; the simulation's chosen:
  ac.v(out)/2 is ngspice/ac.v(out)/2, ngspice/... and coax_rlgc:... as typed; the first
  graph edited to 3*L keeps coax_rlgc; OK, and the diagram draws each (7.5e-7, 2πfL, 50 Ω, 0.5
  and 2, ...), the red one nothing and why.
- test_qucs_control aDataFileIsImportedAndPlotted: an imported dataset's expression traced,
  11 points; one that does not evaluate, why; under Qucsator out.V/2 + time traced, 3 points,
  the note. anExpressionTraceIsComputed: under Qucsator v(out)/2 is traced, the note says the
  diagram evaluates it (it was refused, an Eqn asked for: the test changed with the behaviour).

The Release suite 126 of 126; test_dataset, test_data_import, the two test_binary_dataset
functions and aDataFileIsImportedAndPlotted under the sanitizers, no report.

Twenty-four changes broken on purpose, each caught - two after the dialog's test was made to
see them:

| Broken | Caught by |
|---|---|
| the graph not evaluating an expression | aGraphOfAnExpression, anExpressionOverAnImportedDataset |
| loadDatFile's slash split as before | both (the dialog's test crashed on a graph with no data: it now checks each first) |
| independent variables refused | expressionsOfTheSweeps, aGraphOfAnExpression |
| a sweep's stride from the wrong side | both (40, not 20) |
| no pi | expressionsOfTheSweeps |
| pi over a variable so named | expressionsOfTheSweeps |
| a slash inside a name again | expressionsOfTheSweeps, aGraphOfAnExpression |
| no simulator's prefix in a name | expressionsAreEvaluated |
| typed text as typed | anExpressionOverAnImportedDataset |
| an edit taking the dataset chosen | anExpressionOverAnImportedDataset |
| no red | anExpressionOverAnImportedDataset |
| the red check over the dataset chosen, not the graph's | anExpressionOverAnImportedDataset |
| no analysis of an expression's label | aGraphOfAnExpression |
| unprefixed by the last slash | aGraphOfAnExpression |
| a variable so named evaluated | aGraphOfAnExpression |
| the part not taken | aGraphOfAnExpression |
| plotted against ignored | aGraphOfAnExpression |
| an x of a dependent variable refused | aGraphOfAnExpression |
| Claude refusing under Qucsator | aDataFileIsImportedAndPlotted |
| the reason not said | aDataFileIsImportedAndPlotted |
| log2 by the complex log | expressionsOfTheSweeps |
| a label's analysis cut at the first dot | aGraphOfAnExpression |
| typed other:... given the chosen simulator | anExpressionOverAnImportedDataset, after `coax_rlgc:L*1e6` was added |
| the dialog's file list by the first slash | anExpressionOverAnImportedDataset, after the tip of sqrt(L/C) was checked |

## Not done, said

- No derivative, integral or indexing in the expressions (d/dx, a value at a sample): Nutmeg's
  deriv() and the like remain a NutmegEq's.
- An expression graph reads its whole dataset (a variable's reads its blocks alone): a
  2 GB binary run's expression reads it all, once, when it changes.
- The x of an expression is its variables' sweeps or one variable; an expression as the x is
  not taken (`2*L@frequency/1e9` plots against a variable named so).
