# Bug hunt: the plot digitizer's robustness

*12 November 2026 - Qucs-S on `claude/plot-digitizer-12e8ee` (on `e32067a1`), a Release build and an
ASan/UBSan build, macOS 27.2, Qt 6.12. The user: "run a robustness test on the plot digitizer". Found,
fixed and tested here, each fix with a test that fails on the code before it. 13 November: R3, a
regression of R1 that the recheck of the release found on a real paper (the user: "Yes, fix N1"), fixed
here too; and R4, found by the fuzzer's run after it.*

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
| **R3** | R1's budget, the page's between its reads, left a real paper's second plot unread. Xu et al.'s page 5, an ordinary IEEE page of two columns (156,642 points of paths: 638 calls), two plots in one column: the first plot's read took 636 calls - the text between its labels and the next plot's in reading order is the other column's, and was read whole - and the second got none, uncalibrated. Which plot, by the order they were asked in. Found by the recheck of 12 November (its N1) | regression (since R1) | `plotdigitizer.cpp` `Source::tokens` | yes: each read has the page's allowance of its own; past it, the page's text runs are taken into lines, and only the lines meeting the place are read, narrowest first (tick labels, a legend's words), a column's lines last or not at all. Both plots calibrate in either order, as on the build of 9 November (11 and 13 references) - `twoPlotsOfAPageReadInEitherOrder` |
| **R4** | The raster route's follower, looking across a line 2.5 widths each side in half pixels, cut its samples to 512: an even count, so its search outward from the middle read `values[512]` at its last step (UBSan). A line over some 100 pixels thick does it - in the fuzzer's page, bands of dark fill traced as lines | out of bounds (one past a stack array) | `plotdigitizer_raster.cpp:724` `Follower::centre` | yes: cut to 511, odd. `fuzzThePipeline` with no `QUCS_DIGITIZER_FUZZ` now replays the rounds that found a bug - seed 21, round 35 - in every run of the suite: under ASan it stops at the old code |
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
calibration right at the frame's corner, `textCut` set. The test checks the cut and the axes, not the
time alone (a limit of 60 s), so a slower machine does not fail it. (Since R3 the caption's lines, left
unread, are not said: they are no labels.)

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

## R3. The page's budget, between its reads

The recheck of the release (27.1.3, with R1) reran the SOI MMI benchmark's 60 calls: Xu et al.'s page 5,
plot 2, calibrated on 9 November with 13 references, came back with none and R1's note. Plot 1 had 11
references when asked first, and none when asked after plot 2. Reproduced here on a copy of the paper,
with a probe counting the calls:

```
page 4 weight 156642 paths 7797 budget 638
tokens page 4 within 115.35,297.23 358.62x190.82: 636 calls, 0 left, cut 1
tokens page 4 within 114.18,407.75 360.15x190.73: 0 calls, 0 left, cut 1
```

Two causes:

- The budget was the page's, spent by every read and kept with the page's text across the tools' calls
  (the tools hold the last four PDFs). One read could take it all.
- A read asks about every character between the first and the last text run meeting its place, in
  reading order. The place is the frame and 120 points about it; on a page of two columns that takes in
  the other column's lines, and the text between a plot's labels and the next plot's is that column's
  text. The first read spent 636 calls on it.

**The fix** (`plotdigitizer.cpp`, `Source::tokens`):

- Each read gets the page's allowance (R1's `clamp(10^8 / weight, 32, 10^6)`) of its own. What was read
  stays cached, so a later read only costs less.
- A read that fits its allowance reads as before. One that does not:
  - The page's text runs (the boxes `getAllText` gives, no calls) are taken into lines. Two runs are of one
    line when they overlap in height by half the lower's, neither more than twice the other's height, and
    stand within a letter's height. pdfium's runs are a word, or a few letters of one, as the writer placed
    its glyphs. The height rule keeps a turned axis title from swallowing the labels beside it, which had
    lost plot 2's -10 and -15.
  - Only the lines meeting the place are read, narrowest first. A line's letters are found by one call (a
    selection from its one end to the other). The ends pdfium gives are a letter out either way, so two
    more each side are read, and a letter is taken only if its box is on the line.
  - A line is chosen while the estimated calls fit the allowance. A line wider than half the place (and
    120 points) is a column's text: it and the rest are left. Each line is costed as if not yet known, so
    the same lines are chosen whatever was read before.
  - The lines are read narrowest first, so if the calls run out it is a column's text that goes, not a
    label. Their words are then put back in the text's order, where a raised power, a line of its own,
    joins its "10".
- A page past a million points (fewer than 100 calls) still reads in the text's order, as R1 left it:
  finding each line's letters would take more calls than reading them.
- The note says that labels may be missing only when a line under 120 points went unread
  (`Source::textCut(page, &narrowest)`). Lines of text left are no loss.

**Test.** `twoPlotsOfAPageReadInEitherOrder`: a page of two columns, two plots in the right one, 56 lines of
text in the left between the two plots' labels, and 10,000 dots below (251,068 points: 398 calls). Each
plot is found by itself, top first and bottom first, each order with a new `Source`, then both at once;
every time both calibrate, the corners to 1 % of the span, and with no note. On the code of `15f82900`
the plot read second is uncalibrated, with R1's note, in either order. 4.4 s.

**On the paper.** Xu p5/1 then p5/2, and p5/2 then p5/1, each in a new server: 11 and 13 references both
times. The benchmark's 60 calls replayed on fresh copies, the installed 27.1.3 against the fix:
- 59 identical (trust, references, axes, every series' points);
- Xu p5/2: trust none to medium, 13 references, both axes' titles read. Its curves are as on 9 November:
  the first named "(b)" from the panel's label;
- R1's note on no call (two before);
- 20.2 s in all against 23.3 s. Xu p5/1 alone 1.6 s against 3.7 s.

`chen_regression.py` passes.

## R4. One past the follower's samples

The ASan run that followed R3 (seed 21) stopped in its round 35:

```
plotdigitizer_raster.cpp:724:17: runtime error: index 512 out of bounds for type 'double[512]'
    #0 Follower::centre  #1 Follower::coast  #2 Follower::walk  #3 followFrom  #4 extractRaster
```

`centre` samples across the line, `2 * count + 1` samples for a reach of 2.5 line widths, cut to 512. With
the cut the count is even, the middle 256, and the search outward from it ends at `values[mid + mid]`, one
past the end. The page had bands of dark fill under bars, traced as lines tens of points wide. Before
the hunt's pages, no line was that thick; in Release the read is of one double past the array, and its
value goes into the line's middle.

**Fixed:** the cut is 511. **Test:** a thick line drawn for the purpose did not reach `centre` with such
a width, so the fuzzer's own page is the test: `fuzzThePipeline` without `QUCS_DIGITIZER_FUZZ` replays
seed 21, round 35 (no time limit: under ASan the round takes 69 s), and on the old code ASan stops it.
The pages are the generator's, so a change to the generator changes them: the comment says so.

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
