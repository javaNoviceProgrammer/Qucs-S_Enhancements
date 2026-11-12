# Bug hunt: the plot digitizer's robustness

*12 November 2026 - Qucs-S on `claude/plot-digitizer-12e8ee` (on `e32067a1`), a Release build and an
ASan/UBSan build, macOS 27.2, Qt 6.12. The user: "run a robustness test on the plot digitizer". Found,
fixed and tested here, each fix with a test that fails on the code before it.*

The digitizer already had fuzzers for its parts: the PDF reader on damaged bytes (`fuzzTheReader`), its
content streams of operators in any order (`fuzzTheContentStream`) and the record kept beside a PDF
(`fuzzTheSavedFile`). None ran the engine itself, from a page to the numbers. This hunt adds two fuzzers
that do:

| # | What | How | Size | Build |
|---|---|---|---|---|
| 1 | The engine, whole | `test_plot_digitizer fuzzThePipeline` (`QUCS_DIGITIZER_FUZZ=<seconds>`, new). A page drawn from the round's seed: plots of any frame (none, a point, past the page), ticks and labels of any text (`1e999`, `×10³`, `10⁻⁹⁹⁹`, 26 nines), curves by the hundred or of 50,000 points, markers, bars, areas, black arrows and rings, legends, text strewn, pictures of noise, the plot turned. Three rounds in ten wild; the rest ordinary plots with a hostile thing or two, since most wild pages have no plot to find. Written as vector art or as a scan (turned, sheared, dark, speckled, 12 to 200 dpi). Then: `findPlots` on each page (and on pages -1 and past the end), `plotInRegion` on any rectangle, `calibrateFromAxes`, references of any value and kind, `assignTickValues` of any values, `extract` with any options (route, colours, seeds, counts, scales including -3, 1e6 and NaN), `traceFrom` any point, the hand's edits (points and breaks of any value), then `assess`, `values`, `thinned`, `csv`, `dataset`, the record and back, `overlay` and `paintMarks`. A call past `QUCS_DIGITIZER_FUZZ_SLOW` ms (the drawing not counted) fails the run, its PDF kept; `QUCS_DIGITIZER_FUZZ_ROUND` replays one round | Release: 4,006 rounds in three runs of 10 to 25 min (seeds 3, 6, 9; 4,910 pages, 4,150 plots found, 2,769 extracted, 14,844 series); ASan: 305 rounds in three runs (seeds 2, 4, 5) | both |
| 2 | The tools | [`fuzz_digitizer_tools.py`](2026-11-12-digitizer-robustness/fuzz_digitizer_tools.py) (new; the client of the crash-hardening hunt's `mcp.py`): `find_plots`, `digitize_plot` and `get_digitized` over `qucs-s --mcp-server` with arguments from their schemas and past them - wrong types, numbers huge, negative and out of range, regions inside out, references on one point or 50 of them, colours that are not colours, outputs into folders, missing folders and `/dev/null`, `keep` and `replace`, `show` with no window. Six calls in ten so (one argument in five of any type in four of them); four in ten clean, a good PDF's first plot with two or three valid options, so that the call goes all the way. On the test suite's 45 PDFs and two of the fuzzer's (one of 1.5 million points), 12 damaged copies, an empty file, a text file, a folder named `.pdf`, a missing file and paths outside the workspace; kept records damaged beside six of them (not JSON, a page of -5, a version of 99, 100,000 brackets) | 7,900 calls in six seeds on ASan, 420 on Release | ASan |

## Summary

| | Finding | Kind | Where | Fixed |
|---|---|---|---|---|
| **R1** | A page heavy with paths makes reading its text take minutes. `Source::tokens` asks pdfium for a box per character or word, and QtPdf's `getSelectionAtIndex` loads and parses the whole page at each call, so a call costs about 150 ns per point of the page's paths. The fuzzer's page of 400 curves and 60,000 markers (1.5 million points, 8 MB): 0.23 s a call, 162 calls for one plot's labels, `findPlots` 53 s and the round 77 s. A paper's dense scatter plot is such a page | hang (minutes; the GUI thread in the panel) | `plotdigitizer.cpp` `Source::tokens` | yes: each page gets 10^8 / weight calls (weight = its points + 16 a path), at least 32: about 15 s of pdfium at most. Past a million points every word is read by its box, one call each, so the tick labels still fit; what goes unread is said in the plot's notes. The fuzzer's page 77 s to 15 s - `aHeavyPageIsReadInTime` (120,000 markers: 49 s and the text read whole before, 9.8 s now, the axes read, the cut said) |
| **R2** | `Options::maxSeries` at `INT_MAX`: `mostTracks * 6` overflows (UBSan, the raster route); a negative count kept every series and counted more left out than were found, 0 kept none | undefined behaviour | `plotdigitizer_raster.cpp:2562`, `plotdigitizer_trace.cpp:1026` | yes: clamped to 1 to 1,000 where `extract` begins - `anyCountOfSeriesIsTaken`. The panel and the tools always ask for 24; only the engine's callers could give another |
| N1 | Even with R1's budget a heavy page's text costs seconds: `findPlots` 7 s on the fuzzer's worst page on Release (4.7 s of it in pdfium). QtPdf has no way to keep a page loaded between calls, and its pdfium is not exported to call directly | note | QtPdf | - |
| N2 | Under ASan the raster route at its 16-megapixel cap runs 40 to 100 times slower than Release: single calls of 126 to 377 s (the same calls 2.4 to 7.4 s on Release); through the tools `digitize_plot` with `trace_scale` 16, 101 and 178 s (3.5 s on Release). The ASan runs' limit was 150 s, Release's 20 s | note (ASan only) | - | - |
| N3 | R1's first budget broke a real plot: past 50,000 points it read every word whole, and the page 14 of the Chen et al. paper (60,030) lost its raised powers ("5x10" and a raised "14"), four curves unnamed. `chen_regression.py` caught it before anything else. Whole words now only past a million points; the page uses 585 of its 1,666 calls | caught in the hunt | - | - |
| N4 | The fuzzer's own drawing: QPainter stroking a 60-point-wide or finely dashed pen along a long scribble on a picture took minutes (Qt's rasterizer, not the digitizer). Such pens are kept to short lines | the fuzzer | - | - |

## R1. A heavy page's text

The fuzzer's round 8 (seed 0) drew one plot with about 400 curves of markers. `findPlots` took 53 s.
`sample` showed nearly all of it in `QPdfDocument::getSelectionAtIndex` → `FPDF_LoadPage` →
`CPDF_StreamContentParser::Parse`: pdfium parsing the page's content stream again for every question.
Counted with a probe in `Source::tokens`:

```
page 0: 62728 paths, 1517475 points, a call 227 ms
tokens: 162 calls, 42234 ms
```

`tokens` asks one call per word, and one per character for short words (most tick labels) and where a
power of ten may be raised. A page of 20,000 points costs about 3 ms a call, so nothing showed before. A
page of a dense scatter plot costs 50 to 100 times that.

**The fix** (`plotdigitizer.cpp`):

- When a page's text is first read, its weight is taken from the vector reader's paths, already parsed:
  each point, plus 16 a path.
- The page gets `clamp(10^8 / weight, 32, 10^6)` calls, at about 150 ns a point a call some 15 s at most.
  A paper's page of plots weighs 5,000 to 70,000 points, so it gets 1,400 calls and more. The two plots
  of Chen et al.'s page 14 ask 585.
- Past a million points (fewer than 100 calls) every word is read by its box, one call each, so a plot's
  tick labels fit the budget. A power raised in one word is then lost.
- When calls run out, what is left unread is skipped. `Source::textCut(page)` says so, and `findPlots`
  adds to the plot's notes: "The page is so heavy with paths that its text was read only in part:
  labels may be missing. 'references', or 'x_range' and 'y_range', calibrate it."

**Test.** `aHeavyPageIsReadInTime`: a plot with 120,000 grey markers about it (1.08 million points) and a
caption of short words under it. Before: 49 s, and the text read whole. After: 9.8 s; both axes read, the
calibration right at the frame's corner, `textCut` set and the note there. The test checks the cut and
the axes, not the time alone (a limit of 60 s), so a slower machine does not fail it.

## R2. Any count of series

The ASan run of seed 2 stopped after 13 rounds:

```
plotdigitizer_raster.cpp:2562:54: runtime error: signed integer overflow: 2147483647 * 6 cannot be represented in type 'int'
    #0 extractRaster plotdigitizer_raster.cpp:2562
    #1 extract plotdigitizer_trace.cpp:1541
```

`mostTracks = max(1, maxSeries)`, then `mostTracks * 6`. At the end both routes keep `found.mid(0,
maxSeries)` and count `found.size() - maxSeries` left out: a negative count keeps everything and counts
too many, 0 keeps nothing. `extract` now takes a copy of its options with `maxSeries` clamped to 1 to
1,000.

**Test.** `anyCountOfSeriesIsTaken`: a plot of two curves extracted by each route with `INT_MAX`,
`INT_MIN`, -1, 0, 1 and 24; each gives a series or two, never more than it asked, and counts 0 to 2 left
out. Without the clamp it fails on Release (-1 counts three left out).

## What was found right

- No crash, hang or sanitizer report in 8,320 tool calls, the three tools' answers all a result or a
  reason. Refusals were the argument checks working ("page is a whole number, not a list", "has pages 1
  to 1", "'region' is of a page: give 'page' too", "cannot be read as a PDF"). A folder named `.pdf`,
  `/dev/null` and paths outside the workspace were refused. A kept record of 100,000 brackets was
  refused, not followed down.
- No crash or sanitizer report from the pipeline but R2: degenerate frames, axes from NaN to infinity,
  labels that are not numbers, references all on one point, polar and projective fits of nothing, seeds
  and edits at ±1e300, scans at 12 dpi, inverted and speckled.
- The raster route's 16-megapixel cap holds for any scale asked (1e6, 1e15): 2.4 s on Release at most.

## Not done

- Linux. The ASan runs were macOS's.
- The panel. Its own fuzzing (a monkey over the viewer) is the crash-hardening hunt's.
- Real heavy PDFs. R1's pages are the fuzzer's and the test's; no paper with a scatter of 60,000
  markers was at hand.
