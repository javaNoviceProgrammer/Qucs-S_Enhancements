# Feature gaps: a job's stage beside its progress bar

*12 October 2026 — Qucs-S 26.1.8.*

Reported in conversation, with a screenshot; there is no review file for it. While a
model solved, the progress bar in the Multiphysics tab's toolbar did not fit its text.
"Vr = 5 V: Mode Analysis: …" was wrapped onto two lines and cut off at the top and the
bottom.

On `edf3416` the bar carried the job's stage and its percentage as one text
(`setFormat(stage + " %p%")`), in at most 200 px at the toolbar's right end. The
designed themes draw it with Fusion, which wraps a progress bar's text that is too long
(a `QTextOption` with word wrap) and clips it to the bar. A sweep's stages are long:
each point's parameters, then the step, then what the step is doing. On macOS's own
style the bar draws no text at all, so there the stage was never seen. Reproduced with
a local build: `gesi_eam.qfem`'s `std2` showed "Va = 0 V, / lambda = 1.51" in two
cut-off lines.

## Now

| | Before | Now |
|---|---|---|
| Where | The bar and Cancel at the toolbar's right end | A row of their own below the toolbar: the stage on the left, the bar and Cancel on the right. The toolbar no longer needs room for them. |
| The stage | Inside the bar, wrapped and cut off | A label on one line, as wide as the window allows: "Va = 0 V, lambda = 1.51 µm: Mode Analysis: modes at 198.5 THz". When it does not fit, it is elided with "…" (shortened again whenever its room changes), and the whole text is in its tool tip. |
| The bar | The stage and its percentage | Its percentage alone ("6%"), in 160 px |
| When | Shown when a job starts | Shown once the job has taken a quarter of a second, so a geometry built in a millisecond no longer makes it flash up and away. Hidden when the job ends. |

The label is the one the status bar's hint uses: one line, elided to its room, whole in
its tool tip. The layout viewer's loading row already keeps its text in a label and its
bar without text.

## Test

**`aJobsStageInItsRow`** (in `test_multiphysics_doc`) computes `gesi_eam.qfem`'s `std2`
(a sweep over `Va` and `lambda`, each point a mode analysis), then checks:

- the row shows, and not before 250 ms;
- once the stage reaches a point of the sweep ("lambda = "), it is shown on one line,
  whole and equal to its tool tip;
- given 100 px, it is elided: it fits, ends in "…" and begins as its tool tip does;
- the bar's text is a percentage that fits the bar;
- after Cancel the row is hidden;
- with the stage then held still, the label is elided again as its room grows and
  shrinks.

**`aJobIsCancelled`** now waits for Cancel to show and clicks it.

Both pass in Release and under ASan/UBSan (the whole `test_multiphysics_doc`, 39
functions, no sanitizer report).

## Breaks

Each break was built and the two tests run against it. All of them failed:

| Break | Fails at |
|---|---|
| The stage not elided | Given 100 px, it does not fit |
| Elided only when the stage changes, not when the label is resized | The stage held still: still elided with room to spare |
| No tool tip | The stage not whole in its tool tip |
| The row shown as the job starts | Shown after 0 ms |
| The row not hidden when the job ends | Cancel still shown; the row still shown |
| The stage in the bar again | The bar's text not a percentage that fits |
