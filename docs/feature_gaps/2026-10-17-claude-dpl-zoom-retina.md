# A data display's lines missing on a Retina screen

The user (2026-10-17): *"the dpl zoom in and out has bugs"*, with a picture of their double quantum well's band diagram (`double_qw_pg1.dpl`) zoomed in. The valence band was missing from 0 to 16 nm and between 23 and 29 nm, where it runs lowest, and every curve stopped at 50 nm on an axis running to 94 nm.

## The fault

Since `755bb00`, the canvas passes over a graph's lines that are out of sight: zoomed in, most of a diagram is off the canvas, and Qt outlines each line whether it is seen or not. A line is drawn when it meets the widget's rectangle, mapped back into the diagram's coordinates through the painter's transform.

That transform, `QPainter::combinedTransform()`, includes the device pixel ratio, but the widget's rectangle was given in the widget's own pixels. On a Retina screen (a ratio of 2) the rectangle mapped back was the canvas's top-left quarter. Every line wholly beyond it was passed over: the curves' lower parts and everything right of the middle. A line crossing its edge was drawn whole, which is why the valence band's steps showed.

At a ratio of 1 (an ordinary screen, and the tests' offscreen platform) nothing was missing. `755bb00`'s test checked its pixels on an image of the canvas, which draws every line, so it did not see it either.

## The fix

The widget's rectangle is now taken in device pixels: its size times the device's pixel ratio, as Qt's transform has it (`Graph::drawLines`). On the user's page, on the Retina display, every line meeting the canvas is now drawn at every zoom:

| Zoom | Before: drawn / in sight (Ec, Ev, wave_e, wave_hh, wave_lh) | Now |
|---|---|---|
| 1 | 208/402, 0/402, 1242/2394, 0/2394, 0/2394 | all |
| 4 | 415/813, 0/813, 2490/4878, 0/4878, 0/4878 | all (a few more: a pen's width about) |

## A test, and CI at a ratio of 2

- **test_canvas_drawing `aZoomedDiagramDrawsEveryLineInSight` (new).** A diagram like the user's (240 by 160, band edges with steps, a curve near the bottom), zoomed to 1, 2, 3.5 and 5, with its middle and each corner at the canvas's middle. In each view, every line meeting the canvas is painted by the widget's own paint, and zoomed in some are still passed over.
- **`test_canvas_drawing_ratio2` (a new CTest entry).** The new test and `aPlotOfManyCurvesZoomsSmoothly` run offscreen with `QT_SCALE_FACTOR=2`, so CI's offscreen runs have a Retina screen's pixel ratio. The fault taken back fails it ("206 painted, 399 in sight"), as it fails on the display.

## Also: the VCSEL example's test split

CI's macOS job at `7c01766` failed: `test_fem_resonator theVcselExample` passed QtTest's 300 s. That runner has 3 processors and runs three test files at a time, and its heavy multiphysics files ran beside each other. The modulator's file took 417 s there, against 226 and 307 s in the two runs before; the resonator's whole file had taken 113 and 158 s.

Locally, with Eigen's LU as macOS 15 has it, the function takes 52 s, the same with and without `7c01766`'s change to that LU, so the change did not slow it.

The function made three full solves of the example. Now `theVcselExample` checks order 1 (20 s here with Eigen's LU) and `theVcselExamplesOtherOrders` checks orders 0 and 2 (33 s), with the same checks. The sanitized build checks order 1 alone, as before (45 s), and skips the second.

## Runs

- **The fault taken back:** `test_canvas_drawing_ratio2` fails; `test_canvas_drawing` at a ratio of 1 passes, as before.
- **Offscreen:** the drawing and diagram tests pass, 13 of 13 entries.
- **On the display:** test_canvas_drawing 32 of 32.
- **Under the sanitizers:** both canvas entries, and the VCSEL functions, with no report.
- **The Release suite:** 128 of 128.
