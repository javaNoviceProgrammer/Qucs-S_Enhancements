# Bug hunt: the presentation viewer stressed, its SVG pictures, its speed

*8 November 2026. Qucs-S 27.0.8 (`de6fa0b`). Built with Homebrew Qt 6.12.0, Release and the ASan/UBSan
Debug tree. Everything found here is fixed in the commit that adds this report. Each fix has a test, and
each fix was broken in turn to see its test fail. The probes, the fuzzer, the deck maker, the timing
script and the break script are in [2026-11-08-pptx-viewer/](2026-11-08-pptx-viewer/).*

The user's request: "Now I'd like you to focus on the pptx viewer. Run stress and fuzz tests on it and see
if it crashes or not. Then try to optimize it as much as you can. Looks like embedded SVG images can't be
rendered. Can you also check that?"

## Summary

| | Finding | Since | Fixed |
|---|---|---|---|
| **S1** | PowerPoint's SVG pictures not drawn. An icon inserted on a Mac keeps its SVG in the blip's extension list (`asvg:svgBlip`), and its blip has no picture of its own: the viewer drew "a picture not in the file" (the user's `Presentation1.pptx`, both slides). PowerPoint 365 keeps a PNG as the blip's own picture beside the SVG; the viewer drew that PNG, blurred when zoomed. | `d37ee17` (the viewer's first commit) | The SVG drawn, from its tree, at any zoom. Else the PNG; else a box saying which |
| **V1** | At 800 % on a screen of two pixels a point, a 16:9 slide is 133 million pixels. That is over the drawing's 120 million cap, so the slide was left **white**, and a repaint was queued every frame for as long as it showed (a busy loop). PowerPoint's largest slide is 56 inches square; at 800 % it is a billion pixels on any screen, with the same result. | `d37ee17` | A slide past 16 million pixels is drawn in 1,024-pixel tiles, only those in sight. A slide that cannot be drawn is not asked for again |
| **V2** | Zoomed out on the last slide, in a view taller than the end of the deck can be scrolled to, the slide before became the slide shown. This is item 4 of the request ("slide 7 at zoom 0.5 became 6" on the screen): it is real. | `1e39afe` (B2's fix kept the slide only when it could be scrolled to the top) | `setZoom()` keeps the slide it was on, as `goToSlide()` already did |
| **V3** | A slide of an extreme aspect made thumbnails of the same aspect. A deck with a 1 × 40,320-point slide made one blank icon of 150 × 6,048,000 pixels (3.6 GB): the MCP server went to 4.7 GB opening it. | `d37ee17` | A thumbnail is at most four times as tall as it is wide, with the slide drawn inside it |
| **V4** | Slide sizes were taken as written. `sldSz cx="99999999999999"` gave a slide of 7.9 × 10⁹ points, and the view's sizes and scroll positions converted doubles past `int` (undefined behaviour). | `d37ee17` | Each side held to 1 to 40,320 points (ten times PowerPoint's largest). The view's conversions are clamped |
| **P1** | Every picture was decoded at reading, at full size, and kept for as long as the deck was open: a deck of 200 photographs took 3.5 s to open and 3.4 GB | `d37ee17` | A picture's header is read when the deck is read. Its pixels are decoded when drawn, shrunk by powers of two to the size drawn, and kept within 256 MB shared by every deck open |
| **P2** | Every part's XML tree was kept for as long as the deck was open (23 October's B3) | `d37ee17` | Let go after reading |
| **P3** | Every slide drawn kept its text layouts: 50 slides of 300 text boxes, 320 MB | `d37ee17` | Kept for the slides drawn last (about 4,000 text frames). A thumbnail keeps none |
| **P4** | The ZIP reader's CRC-32 went a byte at a time: half of opening the photograph deck | before (`zipfile.cpp`) | Eight bytes at a time ("slicing by 8"), the same checksums |
| **P5** | The SVG `<use>` guard looked through every picture's bytes, a megabyte JPEG's too: 9 ms a photograph | `d37ee17` (A3's guard) | Bytes that begin as a PNG, JPEG, GIF, BMP, TIFF or WebP are not looked through |
| **P6** | A relationship was found by going through its part's list, and the loop copied the whole list each time (a range-for over a list shared with the cache detaches it): the square of the slides. A quarter of reading a 3,000-slide deck | `d37ee17` | Looked up by id |
| **P7** | Every ZIP entry's DOS date was made into local time (`mktime`): a fifth of reading a 3,000-slide deck | before (`zipfile.cpp`) | Each distinct date made once (a deck's entries mostly share one) |
| **P8** | Zooming drew the whole slide at the new zoom, 300 MB for a slide at 600 % on a Retina screen | `d37ee17` | Tiles (V1) |
| **P9** | Lazy decoding (P1) moved a photograph's decoding into the next slide's first drawing, and into the thumbnails | this session | The next slides' pictures, and the thumbnails' pictures, decoded beforehand on worker threads |

**No crash, sanitizer report or hang was found by the fuzzers.** Fuzzing covered 2,400 mutant decks under
ASan+UBSan, 1,000 fuzzed calls of Claude's deck tools on bad decks through the real MCP server (ASan), and
calls by hand. The hangs and blow-ups found are V1 (a repaint every frame), V3 (4.7 GB) and the
quadratic reading of P6. They were found by decks made by hand and by timing, not by the fuzzers.

## S1: SVG pictures

The user's `~/Desktop/Presentation1.pptx` (made in PowerPoint for Mac, 8 November) has two slides of
inserted icons. Each picture is:

```xml
<p:blipFill><a:blip><a:extLst><a:ext uri="{96DAC541-7B7A-43D3-8B79-37D633B846F1}">
  <asvg:svgBlip xmlns:asvg="http://schemas.microsoft.com/office/drawing/2016/SVG/main" r:embed="rId2"/>
</a:ext></a:extLst></a:blip>...
```

The blip has no `r:embed` of its own, and `ppt/media/` holds only `image1.svg` and `image2.svg`. The
content type is the `svg` default. Qt Svg was in the build and drew SVGs that are a blip's own picture
(`picturesDrawn` tested that). The viewer simply never looked in the extension list. So:
- the Mac icon became "a picture linked from outside the file, or not in it", a box;
- a PowerPoint 365 SVG (PNG as its own, SVG in the extension) was drawn from the PNG, which blurs as it
  is zoomed.

Now `blipMedia()` reads both. It draws the SVG when it can be drawn: it parses, it is within the `<use>`
guard, and its view box is not empty. Else it draws the blip's own picture, and if neither can be drawn,
a box naming the SVG or the picture. This holds for pictures (`p:pic`), picture fills of shapes, and
backgrounds. An SVG is drawn from its tree into the painter at whatever scale the device has, so it is
sharp at any zoom (the test draws a one-unit line at 8 pixels a point: black in its middle, white
9 pixels off it).

Checked with decks made each way:
- the PowerPoint 365 way (PNG and `svgBlip`);
- the Mac way (`svgBlip` alone);
- another tool's (the SVG as the blip's own);
- a cut-short SVG, an SVG asking for 10⁹ nodes through `<use>`, an SVG not in the file, an SVG linked
  from outside, and an SVG of no size, each with a PNG fallback (the PNG drawn) and alone (a box, and
  why);
- a cropped SVG;
- an SVG as an ellipse's fill and as the slide's background.

The user's own deck draws both icons, in the probe and through `read_pptx`'s image. An SVG whose
`<image>` names a local file (`file:///…`) is refused by Qt Svg 6.12 ("Could not create image"), so a
deck cannot draw a file of this machine through one.

No sample deck was needed from the user: their Desktop deck was the case.

## Crashes and hangs: what ran

**The reader.** [fuzz2.py](2026-11-08-pptx-viewer/fuzz2.py) is 23 October's deck fuzzer with more
mutations:
- SVG pictures: `svgBlip` extensions pointing at missing, wrong (a slide, a layout, a PNG), external and
  hostile parts; 18 hostile SVGs (empty, cut short, of no size, a view box of 10³⁰⁰ or NaN, an external
  image, 20,000 nested groups, a 2.8 MB style sheet, a 3 MB path, a blur of 10⁹, a gradient that names
  itself, a pattern of 10⁻⁹, 100,000 characters at 10⁹ points, a broken gzip, an external DTD);
- odd entry names in the archive (backslashes, `../`, absolute, upper case, 60,000 characters long, a
  right-to-left override, a folder mark), and a duplicated entry;
- a description attribute of 1 MB and 8 MB, and namespaces swapped;
- layouts whose master is a layout or a slide, and a master whose theme is itself;
- a slide listed 2,000 and 6,000 times;
- PNGs and JPEGs with broken headers, cut short, or claiming 0, 65,535 or 2³²−1 pixels a side; EMFs of
  garbage;
- 500 extension lists in a blip, and slide sizes of 0, 1, −1 and 10¹¹.

It runs on [pptx_probe](2026-11-08-pptx-viewer/probe/), which is the reader and its drawing alone (no
window, no test binary). For each slide the probe draws it whole, finds its parts, draws a 256-pixel
tile from its middle at 800 %, and has its pictures readied at thumbnail size (`prepare()`, the call the
worker threads make). It also draws the slide through a painter, and searches the deck. Results:

| run | mutants | code | result |
|---|---|---|---|
| 1 | 600 | before (`de6fa0b`) | no crash, no sanitizer report; slow: a slide listed 6,000 times, 16 s to read under ASan |
| 2 | 800 | after S1 and P1–P2 | the same |
| 3 | 1,000 | after V1 and P3–P5 (tiles and `prepare()` in the probe) | the same: 1,000 of 1,000 drawn, no crash, no sanitizer report; slow under ASan: 18 decks, each a slide listed 6,000 times |

The 6,000-slide decks read in 0.8 s in Release (445 MB), so the 16 s is ASan's overhead. P6 and P7 make
such a deck cheaper still.

**The tab.** New tests, each run in Release and under ASan/UBSan:
- `theTabHoldsUnderFastUse`: 400 random steps on a 30-slide deck of pictures, SVGs, shapes and notes.
  The steps are slides, next and previous, zoom steps, zooms at random from 10 % to 800 %, fits,
  scrolling both ways, regions, pen strokes and undo, the find bar, and reloads, with repaints and event
  processing between them. After each step the slide shown, the zoom and the slide field agree.
- `tabsClosedWhileTheirPicturesAreDecoded`: six decks of 60 pictures each, opened and closed at once,
  four times. Then all are open at once, one is reloaded twice while its thumbnails are being decoded on
  worker threads, and they are closed from the last tab first.
- `absurdSlideSizesInTheTab`: slides of 10¹⁴ EMU, 1 EMU, 0 × −5, 51,206,400 × 1, 1 × 51,206,400 and
  10³⁰⁰ × NaN, each at 10 %, 100 % and 800 %, scrolled to the end, and fit to the slide.
- `aSlideTooLargeForOneImageIsDrawnInTiles` (V1), `aZoomOutAtTheLastSlideKeepsIt` (V2).

The pen was used on a tiled slide too.

**Claude's tools.** 23 October's [fuzz_mcp.py](2026-10-23-presentations-and-guards/fuzz_mcp.py), 250 calls
on each of four decks through `qucs-s --mcp-server` (the ASan build, offscreen): the bad SVGs, a
4,032-point slide of SVGs, a 1 × 40,320-point slide of bad SVGs, and the user's icons. No crash, hang or
sanitizer report. Then [calls-svg.json](2026-11-08-pptx-viewer/calls-svg.json) by hand: `read_pptx` with
images, `show_pptx` at 800 % with a region, `screenshot`, and `export_image` at scale 12 and to PDF. All
were answered; exports past 120 million pixels are refused, with the reason. This run found V3 (4.7 GB).

## V2: does zooming on the screen lose the slide?

Yes. `setZoom()` puts the slide it was on at the view's top. At the end of the deck the scroll bar stops
short of that, and `updateCurrent()` (which the scroll calls) takes the slide a third of the way down the
view, which is then the slide before. At 50 % a 16:9 slide is 270 pixels tall: once the view is taller
than about 440 pixels, the last slide cannot reach the top. The offscreen view of
`aZoomKeepsTheSlideItWasOn` (a 900 × 700 window) was shorter than that, and the session's display was not.

`aZoomOutAtTheLastSlideKeepsIt` uses a 900 × 1,500 window. It failed before the fix ("slide 7 at zoom 1
became 6") and passes now. `aZoomKeepsTheSlideItWasOn` was not run on the real display again (see
below).

## Speed

**How it was timed.** `benchDecksGiven` is a test function that does nothing unless `QUCS_PPTX_BENCH`
names decks. It runs first in `test_pptx_doc`, through ctest (offscreen). It opens a deck in a tab and
paints each step there and then (`repaint()`):
- the first slide;
- 20 jumps through the deck, then back to the first slide;
- 15 steps to the next slide, 120 ms apart (a person's pace);
- zoom steps from 100 % to 800 % and down to 25 %;
- 40 scroll steps at 800 % and 60 at the width of the view;
- the thumbnails: time until all are drawn, and the longest the event loop was held meanwhile;
- memory: resident, against the moment before opening.

`QT_SCALE_FACTOR=2` gives a device pixel ratio of 2, as on a Retina screen. Decks from
[mkdecks.py](2026-11-08-pptx-viewer/mkdecks.py):

| deck | what it is |
|---|---|
| text10 | 10 slides of titles and bullets |
| shapes50 | 50 slides of 300 shapes with text each |
| photos200 | 200 slides, each with a title, bullets and its own 2,400 × 1,600 JPEG (230 MB) |
| text3000 | 3,000 slides of a title and four bullets |

[ab.sh](2026-11-08-pptx-viewer/ab.sh) ran each deck with the sources of `de6fa0b` (with
`benchDecksGiven` added), then with these changes, twice over. The machine was shared with other
sessions' test runs (load average 15 to 30), so each number below is the lesser of the two passes.
Milliseconds unless said.

**Device pixel ratio 1**

| | text10 before | after | shapes50 before | after | photos200 before | after | text3000 before | after |
|---|---|---|---|---|---|---|---|---|
| open | 16 | 7 | 267 | 256 | 3477 | 188 | 429 | 283 |
| first slide | 35 | 19 | 29 | 27 | 12 | 38 | 11 | 14 |
| switch avg | 11 | 4 | 11 | 23 | 3 | 31 | 2 | 3 |
| switch worst | 56 | 15 | 16 | 34 | 4 | 35 | 4 | 5 |
| next slide (paced) avg | 4 | 4 | 5 | 3 | 3 | 2 | 2 | 3 |
| next slide (paced) worst | 8 | 9 | 11 | 5 | 6 | 3 | 3 | 4 |
| zoom avg | 30 | 8 | 25 | 5 | 6 | 4 | 4 | 2 |
| zoom worst | 178 | 45 | 181 | 15 | 35 | 19 | 28 | 8 |
| scroll 800% avg² | 3 | 1 | 1 | 1 | 0 | 1 | 0 | 1 |
| scroll 800% worst² | 13 | 9 | 8 | 8 | 1 | 4 | 1 | 5 |
| thumbnails | 25 | 18 | 423 | 369 | 55 | 1758 | 476 | 478 |
| event loop held | 24 | 18 | 79 | 71 | 55 | 67 | 59 | 60 |
| memory opened (MB) | 2 | 3 | 214 | 216 | 3384 | 453 | 198 | 204 |
| memory at end (MB) | 279 | 100 | 856 | 482 | 3787 | 839 | 871 | 604 |

**Device pixel ratio 2**

| | text10 before | after | shapes50 before | after | photos200 before | after | text3000 before | after |
|---|---|---|---|---|---|---|---|---|
| open | 7 | 9 | 256 | 258 | 4039 | 207 | 450 | 299 |
| first slide | 22 | 21 | 38 | 38 | 26 | 45 | 26 | 20 |
| switch avg | 6 | 7 | 17 | 32 | 12 | 42 | 12 | 8 |
| switch worst | 8 | 11 | 28 | 49 | 16 | 52 | 17 | 10 |
| next slide (paced) avg | 4 | 5 | 5 | 6 | 7 | 13 | 9 | 9 |
| next slide (paced) worst | 6 | 9 | 6 | 10 | 10 | 36 | 17 | 35 |
| zoom avg | 75 | 15 | 63 | 15 | 19 | 7 | 16 | 7 |
| zoom worst | 374 | 46 | 286 | 35 | 95 | 23 | 68 | 18 |
| scroll 800% avg² | 1 | 3 | 2 | 5 | 2 | 2 | 5 | 3 |
| scroll 800% worst² | 2 | 12 | 5 | 21 | 16 | 10 | 12 | 16 |
| thumbnails | 25 | 24 | 448 | 428 | 86 | 1941 | 647 | 626 |
| event loop held | 24 | 24 | 85 | 81 | 72 | 80 | 71 | 67 |
| memory opened (MB) | 8 | 8 | 220 | 216 | 3385 | 467 | 200 | 205 |
| memory at end (MB) | 629 | 239 | 1356 | 683 | 4084 | 1217 | 1790 | 1204 |

² Before, at ratio 2, a white slide (V1): nothing was drawn at 800 %.

"switch" is a jump through the deck to a slide not shown yet; "next slide (paced)" goes one slide on, 120 ms apart; memory is resident memory against the moment before opening ("opened": after the first slide; "at end": after every step).

What the numbers show:
- **Opening a deck of photographs:** 3.5–4.0 s to 0.19–0.21 s, and 3.4 GB to 0.45 GB (the pictures'
  own bytes). On a 3,000-slide deck: 0.43–0.45 s to 0.28–0.30 s.
- **Zooming:** the worst step 3 to 8 times quicker (374 to 46 ms on a Retina-sized view). Slides at
  600–800 % are drawn in tiles instead of whole.
- **800 % on a Retina-sized view:** before, the "scroll" numbers were of a white slide (V1). Now the
  slide is drawn, at 2–5 ms a step.
- **Memory at the end:** 36–71 % lower on every deck.
- **Costs, kept on purpose.**
  - A jump through the photograph deck to a slide not yet shown decodes its photograph: 3 to 31 ms
    (12 to 42 at ratio 2). Going on to the next slide at a person's pace stays at 2–3 ms, because that
    photograph was decoded meanwhile (P9).
  - The thumbnails of the photograph deck take longer to fill, because their photographs are decoded,
    not already in memory (the 3.4 GB). It was 1.8 s on one worker thread; on up to four threads it is
    **0.39–0.45 s** (measured after the table, two runs at each ratio). The event loop is held no
    longer than before (67–92 ms, one thumbnail's drawing and the timer's 25 ms).
  - On shapes50, jumps cost 23 ms against 11 ms. Before, drawing the thumbnails had kept every slide's
    text layouts (that is the 856 MB). Now they are made again, which is P3's trade.

The reader alone ([pptx_probe](2026-11-08-pptx-viewer/probe/) `--bench`, Release, the best of six runs
in two batches, on the shared machine). "Drawing every slide" draws each slide once at 72 dpi, a pass
the tab never makes:

| deck | read before | after | memory after reading, before | after | after drawing every slide, before | after |
|---|---|---|---|---|---|---|
| photos200 | 3,937 ms | 242 ms | 3,420 MB | 485 MB¹ | 3,446 MB | 698 MB |
| text3000 | 540 ms | 343 ms | 226 MB | 231 MB | 364 MB | 317 MB |
| shapes50 | 326 ms | 293 ms | 238 MB | 238 MB | 563 MB | 385 MB |
| text10 | 53 ms | 53 ms | 30 MB | 30 MB | 45 MB | 46 MB |

¹ Of which 230 MB is the probe's own copy of the file; the tab lets go of the file's bytes once read.

Drawing every slide in a row is slower on shapes50 (642 to 749 ms in the quieter batch, twice in the
busier one): the text layouts of the slides passed are freed as it goes (P3). On photos200 it is 0.4 s
against 3.2 s, because each photograph is decoded as its slide is drawn instead of all at reading (and
the reading itself is 3.7 s quicker).

**Profiled with `sample`:**
- the photograph deck's reading: decoding (P1), then the CRC (P4), then the `<use>` guard (P5);
- the 3,000-slide deck's reading: the relationships (P6), then the dates (P7);
- drawing a photograph slide: libjpeg's Huffman decoding, which no scaled decoding avoids, hence P9.

xctrace was not used.

**Not done:**
- Drawing slides on another thread. The view still draws the slide in sight on the GUI thread (a quarter
  second a frame at most, the rest the next frame). Its text layouts are kept per text frame with no
  lock, so a drawing thread would need one per slide. Only pictures are decoded off the GUI thread.
- Reading a deck on another thread: opening is now quick enough (0.2–0.3 s on these decks).
- Stored (uncompressed) entries could be shared with the file's bytes instead of copied.

## Tests

New in `test_pptx_doc` (48 functions, all passing in Release and under ASan/UBSan through ctest):

| test | catches |
|---|---|
| `svgPicturesDrawnAsPowerPointKeepsThem` | S1: the extension ignored, a broken SVG not fallen back from, a tile drawn at the wrong place |
| `picturesDecodedAsLargeAsDrawn` | P1: colours at five scales, an edge sharp at each, a broken PNG a light box; `prepare()` on four threads while drawing gives the same image |
| `aTileIsThatPartOfTheSlide` | the tile API (`render(index, scale, area)`, `pixels()`), against the whole slide |
| `textLaidOutAgainComesOutTheSame` | P3: a slide drawn again after 39 others, and its parts, the same |
| `aZoomOutAtTheLastSlideKeepsIt` | V2 |
| `aSlideTooLargeForOneImageIsDrawnInTiles` | V1: red in the middle of a 4,032-point slide at 800 %; then nothing drawn again frame after frame; the corner; the pen |
| `theTabHoldsUnderFastUse`, `tabsClosedWhileTheirPicturesAreDecoded` | stress |
| `absurdSlideSizesInTheTab` | V3, V4 |
| `benchDecksGiven` | (timing, skipped unless asked) |

[breaks.py](2026-11-08-pptx-viewer/breaks.py) undoes each fix in turn and runs the tests through ctest:

| undone | caught by |
|---|---|
| the SVG in a blip's extensions ignored | `svgPicturesDrawnAsPowerPointKeepsThem` |
| a broken SVG not fallen back from | `svgPicturesDrawnAsPowerPointKeepsThem` |
| a slide drawn whole at any size (no tiles) | `aSlideTooLargeForOneImageIsDrawnInTiles` |
| a zoom lets the slide shown slip | `aZoomOutAtTheLastSlideKeepsIt` |
| thumbnails as tall as the slide's aspect | `absurdSlideSizesInTheTab` (after its tall slide was added: the first run of breaks.py had no tall slide, and missed it) |
| pictures decoded far smaller than drawn | `picturesDecodedAsLargeAsDrawn` |
| a tile drawn at the slide's top left | `aSlideTooLargeForOneImageIsDrawnInTiles`, `aTileIsThatPartOfTheSlide`, `svgPicturesDrawnAsPowerPointKeepsThem` |
| layouts let go and not made again | `textLaidOutAgainComesOutTheSame`, `theRawInputDrawsOnASlide` |
| slide sizes not held | `absurdSlideSizesInTheTab` |

P4–P7 change no output. `test_zip_doc`'s new `theChecksumIsZipsCrc32` checks P4 against the
standard's check value and a byte-at-a-time checksum, at every length from 0 to 70. P5–P7 are covered
by all the reading in the suites: a picture taken for an SVG, a wrong relationship or a wrong date
would each show there.

## Not verified

- **On the real display.** Everything ran offscreen, and a ratio of 2 was `QT_SCALE_FACTOR=2`, not a
  Retina screen. V2 is shown with a tall offscreen window rather than on the screen where it was seen.
  `aZoomKeepsTheSlideItWasOn` was not run on Cocoa again, by the rule.
- **Thread safety beyond ASan.** `prepare()` (pictures decoded on worker threads) was exercised by a test
  on four threads and by the tabs' prefetch, under ASan. ThreadSanitizer was not run. What it touches is
  locked (each picture's decoding and its SVG's drawing, and the shared cache), or immutable after
  reading.
- **SVG features Qt Svg does not draw** (it is SVG Tiny 1.2 with some extensions: no filters, CSS
  partially). PowerPoint's icons are plain paths and fills, as the user's are. A more elaborate SVG may
  draw differently than in PowerPoint; one that does not parse falls back to its PNG.
- **EMF and WMF** pictures are still a box, as before.
- **Windows and Linux**, and Qt other than 6.12.0.
- The whole Release suite ran (178 of 180 pass). `test_library_export` does not build here (no
  `dos2unix`, known). `test_process_console` fails its Python and terminal-shell checks the same way on
  the main checkout's own build (`1d154f2`), so it predates this work. Under ASan/UBSan, the suites
  that use the changed code ran: `test_pptx_doc`, `test_zip_doc`, `test_data_import`,
  `test_data_export`, `test_spreadsheet`, `test_sheet_doc` and `test_mcp_server`.

## Symbol fonts (9 November)

The user asked whether the viewer handles symbol characters and glyphs. `mksym.py <out.pptx>` makes a
deck of three slides to check:
- runs in Symbol, Wingdings, Wingdings 2 and 3, Webdings and Marlett, as bytes and as the private-use
  characters U+F0xx;
- Unicode: Greek, maths, arrows, dingbats, emoji, Chinese, Japanese and Korean, Arabic, Hebrew,
  Devanagari, Thai, accents and ligatures;
- bullets in symbol fonts.

It was drawn with `renderDecksGiven`.

**What already worked.** All the Unicode text drew correctly, including emoji sequences and
right-to-left scripts. Only ₿ was missing from the fallback fonts. Wingdings, Wingdings 2 and 3, and
Webdings also worked, both as text and as bullets, private-use characters too: macOS ships those
fonts, and Core Text maps U+F0xx into them.

**What did not.** Symbol. The Symbol macOS ships is a Unicode font: its "a" is an "a". PowerPoint reads
Symbol as Adobe's Symbol encoding, where "a" is alpha. Four things went wrong:
- `abg` in Symbol was drawn "abg", not αβγ;
- U+F061 (how PowerPoint often saves it) was drawn as an empty box;
- a run's `<a:sym>` was not read at all;
- Symbol's "·" bullet was drawn as a middle dot. The old bullet table applied only where the font is
  missing, and it never is on a Mac.

**Now** (`pptx.cpp`, `symbolPieces` and `bulletChar`):
- **Symbol.** Its bytes, and U+F020–F0FF in the font named by the run's `<a:sym>` (else by its own
  font), are read through Adobe's Symbol table and drawn in the text's font. This applies whatever is
  installed.
- **`<a:sym>`** applies only to private-use characters, as in PowerPoint. Latin-1 in a run that names
  only `<a:sym>` is drawn as it is.
- **Wingdings** is drawn by its own font where installed (macOS, Windows). Where it is missing (most
  Linux systems), it is read through a table of 162 of its characters, Unicode's nearest:
  smileys, ticks and crosses, boxes, arrows, circled digits, the zodiac, clock faces. Characters not
  in the table are drawn as they are, and the missing font is noted as before.
- **What the text says.** read_pptx, search and the slide's paragraphs see what a symbol stands for,
  even where its font draws it: ☺ for Wingdings' J, ➢ for its Ø bullet.
- **Line height, bullets and empty paragraphs** of a run in a symbol font use the text's font, so a
  missing Symbol or Wingdings on Linux is not reported where nothing is drawn in it.
- **Not covered:** Wingdings 2 and 3 and Webdings have no table. Where they are missing they are drawn
  as letters, with the missing-font note, and their bullets as "•". Neither is Marlett, nor any other
  symbol font.

**Test:** `symbolFontsReadAsWhatTheyStandFor`.
- It checks each paragraph's text: αβγΩ, xα≥√, ÒÓ, "• dot", "• pua", ☺✓, "➢ arrow".
- It checks that no note names Symbol or Wingdings.
- It checks that Symbol's letters and its private-use characters draw exactly as the same Greek in
  Unicode, pixel for pixel.
- It checks that β is found on all four slides.
- It fails on the code before the fix ("abgW" for αβγΩ).
- It also passed with Wingdings treated as missing (a throwaway edit, reverted). The deck drawn that
  way showed the table's symbols.

## Reproducing

Run the scripts with `python3 -B -I`, from a scratch folder.

1. `mkdecks.py <out>` makes the timing and SVG decks (`sips` makes the JPEGs).
2. Build the probe: `cmake -S probe -B b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$(brew
   --prefix qt) -DQUCS_SRC=<qucs-s-26.1.1/qucs>`. Add the sanitizers through `CMAKE_CXX_FLAGS` and
   `CMAKE_EXE_LINKER_FLAGS`. Run it as `pptx_probe [--bench] [--scale S] [--out dir] deck...`.
3. The fuzzer:
   - `fuzz2.py make <fuzz_pptx.py of 23 October> <seeds> <out> <count> <seed>`;
   - `fuzz2.py run <fuzz_pptx.py> <pptx_probe> <out> <findings> [batch] [timeout] [MB]`.
   The seeds were the SVG decks, `text10`, the user's icons deck, three decks from the user's Desktop
   (charts, tables, an EMF), and the two bad decks of the MCP run. Seeds 101, 202 and 303. The mutants
   are not kept: they hold the user's decks.
4. Timing: `S=<folder with decks/> bench.sh <build tree> <out>`, and `ab.sh` (it swaps the sources of
   `de6fa0b` in and out of a tree: run it only on a clean tree). `abtable.py <folder>` makes the
   tables from its passes.
5. `breaks.py <worktree> <build tree>`.
6. `mksym.py <out.pptx>` (next to `mkdecks.py`, which it uses) makes the symbol-font deck. Draw it with
   `QUCS_PPTX_RENDER=<deck> QUCS_PPTX_OUT=<folder>`, through ctest.
