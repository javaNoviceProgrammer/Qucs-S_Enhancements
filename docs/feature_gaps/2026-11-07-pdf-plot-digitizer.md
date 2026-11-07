# A plot digitizer in the PDF viewer, and tools for Claude to use it

The user (2026-11-07): *"Many times I need to ask Claude to digitize some plot in a pdf file. Can you add a feature to the
pdf viewer so that it has a powerful plot digitizer and Claude can quickly use it to digitize a plot? Do this in a new
session."*

*Apple M2 Ultra, macOS 27.2, Qt 6.12.0 from Homebrew. CI and the published releases use Qt 6.10.3 with qtcharts and qtpdf.
Nothing here was built or run against 6.10.3; see "What could not be verified".*

**Now.**
- **In the viewer:** a **Digitize** button beside Find opens a panel at the side of the pages. **Find Plots on This Page**
  frames each plot, calibrates it from its tick marks and labels, and extracts its curves and markers, named from the
  legend. The panel also has Mark Region, Add Reference, Follow Line, Pick Colour, Add Point, Move Point and Delete Point,
  a magnifier by the pointer, Undo (Ctrl+Z), and Copy, CSV… and Dataset…. Everything extracted is drawn over the page.
- **Kept with the PDF:** each change is written beside the PDF as `<name>.pdf.digitized.json`. Reopening the PDF opens the
  panel on what was kept.
- **For Claude:** three tools that work with no window (`--mcp-server`):
  - `find_plots` lists a document's plots.
  - `digitize_plot` returns, in **one call**, every series in the axes' units, how far to trust them, the time taken, and a
    picture of the plot with the extraction drawn over it.
  - `get_digitized` reads what the user digitized by hand, or what Claude kept.

![The panel on a real display, dark theme: the extracted curves drawn over the page in their complementary colours, the tick references as crosses](2026-11-07-pdf-plot-digitizer/panel-real-display-dark.png)

**What falls short of the request:**
- **No automatic calibration of a polar plot.** A polar plot (and a Smith chart, which is not supported at all) is
  calibrated from three or more references: points with known r and angle, clicked in the panel or given to
  `digitize_plot`. Its grid circles and spokes are then recognised and left out.
- **No OCR.** A scan whose labels are only pixels gets its frame and tick marks found, but not their values. Claude
  supplies the values (`x_values`, `y_values`, one per mark the first answer lists) from the picture it receives; a user
  types them into the reference table. Reading the labels would need an OCR engine (Tesseract, or Apple's Vision on macOS
  only). That would be a new dependency, so I did not add one; whether to is the user's call.
- **No encrypted files on the vector route.** Their paths are not read: a PDF with any `/Encrypt` goes to the raster route,
  and the answer says why. The standard security handler would need RC4/AES and key derivation, which I did not write.
- **Axes without a box are not found automatically.** Detection needs a frame, or at least two edges meeting at a corner
  (an L of spines). Spines detached from each other, and axes through the middle of the plot, are digitized by region plus
  references.
- **A second y axis is noticed, not used.** Labels on both sides that disagree produce a note saying which side the
  numbers use. A curve read against the other axis needs references from that axis.
- **Some extraction gaps:**
  - Error bars are not extracted.
  - Hollow markers sitting on a line are not separated from the line on the raster route. Hollow markers alone are found.
  - Overlapping curves of one colour are followed by direction, which works at crossings but not where two curves run
    together.

## Where the numbers come from

**Vector art.** `QPdfDocument` draws pages and gives their text, but does not expose the paths: QtPdf exports none of
pdfium's `FPDF_*` symbols (checked with `nm` on Homebrew's QtPdf). So `pdfvector.{h,cpp}` is a small reader, about 1,000
lines:
- **Finding objects:** cross-reference tables and streams, `/Prev` chains, hybrid files and object streams. Where the
  cross-reference is wrong or missing, it falls back to scanning the file for `N G obj`, with the later object of a number
  winning.
- **Decoding streams:** Flate (through the inflater `zipfile.cpp` already has), LZW, ASCII85, ASCIIHex, RunLength, and the
  PNG and TIFF predictors.
- **The page:** its crop box and rotation, and its content stream. It follows paths, `cm`, `q`/`Q`, colours in every colour
  space (approximately for Lab, Indexed and Separation), line width and dash (from `gs` too), clipping (as a bounding box),
  and form XObjects (with the origin recorded: matplotlib's markers are forms, and the origin is the data point). It records
  where images are placed.
- **Limits:** every length, count, depth and nesting has one, and a page stops after 30 s.

Where the page is vector art, the series are **the file's own vertices**. A cheap check runs on every vector series: the
share of its points that have ink under them in pdfium's own picture of the page (`ink_under_points`). That catches any
case where this reader and pdfium disagree about the page. The reader's page size is also compared with pdfium's and
rescaled to it (which covers `/UserUnit`).

**Pictures.** Embedded images and scans go to the raster route (`plotdigitizer_trace.cpp`):
- **Rendering:** the page is drawn at the scan's own resolution (4 to 10 pixels per point, at most 16 MP).
- **Straightening:** a turned page is set straight, by the angle where its dark pixels pile up in rows.
- **Clearing the clutter:** grid rows and columns at tick marks, inner ticks, and words that pdfium knows are on the plot
  are removed.
- **Sorting by colour:** pixels go into colour families by the direction of their difference from the background. An
  anti-aliased edge has the same direction as the line's core, so it joins the right family.
- **Markers:** small compact blobs of one size, with a moment test that keeps slanted dashes out. Markers sitting on a line
  are found as patches thick in both directions.
- **Lines:** each line is followed with momentum. At every step it takes the ink-weighted middle across the line (to a
  fraction of a pixel). It goes straight through crossings and marks, and jumps dash gaps and occlusions within a narrow
  fan. It makes no points inside gaps, at a cut-short dash end, or where it lands after a jump.

**Calibration** (`Calibration` in `plotdigitizer.h`):
- **The fit:** least squares from references, linear in x and y or in their logarithms. Each reference carries x, y or
  both.
- **Ticks along one edge** only fix the direction along that edge, and are fitted that way. Three or more references off a
  line fix a full affine map, which handles turned and sheared pages. Two opposite corners alone are taken as axis-aligned.
- **Other models:** projective (four or more references) for a page photographed at an angle, and polar with r's value at
  the centre (a pattern in dB).
- **Residuals:** reported per reference, with their rms.
- **Automatic calibration:**
  - Labels are read as numbers: `−1.5`, `1e-3`, `10^3`, a raised `10` plus exponent, `2×10⁵`, `100k`, `10µ`, `1,000`,
    `0,5`, `2.4GHz`, `50%`, `-3dB`.
  - Each label is snapped to the tick mark or grid line under it.
  - Linear and log are both fitted robustly, so a page number or a stray label among the ticks is dropped and reported.
  - A `×10⁻³` or `1e6` at the end of an axis is applied, with a note.
  - Two labels alone cannot tell linear from log. The spacing of the unlabelled marks between them decides, and the answer
    says it is a guess.

**How far to trust it** (`assess()`): returned with every answer and shown in the panel.
- **A level:** high, medium, low or none, with the reasons.
- **What a point of the page is worth** on each axis (decades per point on a log axis).
- **What each value is good to:** the calibration rms combined with the route's own error. The file's vertices count as
  0.02 pt. The raster route counts as 1.5 px at its scale, which is about what the tests below measure.

## Accuracy measured

`qucs/tests/test_plot_digitizer.cpp` writes PDFs with QPdfWriter from known functions and compares each extracted value
with the function at that x. Errors are in percent of the axis span; the frames are 400 × 280 pt, so 0.06 % of the y span
is 0.17 pt.

| Case | rms | worst |
|---|---|---|
| Vector, linear (two curves, legend, grid) | < 0.00001 % | < 0.00001 % |
| Vector, log-log; reversed x with "1k" labels; dashed; 19 markers | < 0.00001 % | < 0.00001 % |
| Vector polar pattern (by references) | 0.0000 dB | 0.0000 dB |
| Picture, 300 dpi, two curves crossing, grid | 0.061 %, 0.065 % | 0.19 %, 0.22 % |
| Picture, 17 markers | 0.063 % | 0.11 % |
| Picture, dashed line (gaps followed) | 0.072 % | 0.51 % |
| Scan turned 2.4°, no text (tick values given) | 0.066 % | 0.13 % |
| Two black lines crossing, same colour | 0.070 %, 0.063 % | 0.50 %, 0.26 % |
| A hysteresis loop (x goes both ways) | | 0.09 % of the span |
| Polar pattern in a picture, 40 dB span | 0.040 dB | 0.14 dB |
| Scan sheared 6 % and turned 3°, by four references | 0.37 % | 0.70 % |

- **The sheared scan's error is pdfium's, not the trace's.** Its frame's corners in pdfium's picture are 0.7 to 2.1 points
  from where the file's image matrix puts them (measured by rendering the corners). The test's references use the file's
  matrix. References clicked on the picture would agree with it. A rotation alone is drawn correctly, as the turned case
  shows.
- **Worst errors on the raster route come from crossings and dash ends,** where the line under the trace is partly covered.
- **Other measurements:**
  - Calibration residuals are below 0.05 pt on vector pages and below 0.3 pt on pictures.
  - Tick positions in pictures are found to a fraction of a pixel. Before that was done, they were whole pixels and the rms
    was 0.10 %.
- **Real PDFs:**
  - **From the repository:** `qucs/python/qucs-py/test1dat.pdf`, a log-log plot of six swept curves exported by Qucs (each
    curve written as ten separate two-point paths). It is found, calibrated to 0.27 pt rms from its labels, and gives six
    curves of ten points each, all with ink under every point. The 0.27 pt is Qucs's own rounding: its bottom label "0.003"
    sits 0.7 pt from where 0.003 falls. `test1.pdf` next to it is a schematic with no plot.
  - **matplotlib** (3.11, out of tree, PDF font types 3 and 42; not checked in, since they are not ours to vendor): linear
    and log-log plots with a legend, a dashed curve and square markers come back named and exact to a millionth.

## Time per call

`2026-11-07-pdf-plot-digitizer/mcp_times.py` runs the program as `--mcp-server` with no window and times each call as the
client sees it, over stdio, picture included:

| PDF | find_plots | digitize_plot (first, again) | in `ms` |
|---|---|---|---|
| matplotlib, linear, 3 series | 65 ms | 34, 30 ms | 11 ms of work |
| matplotlib, log-log | 15 ms | 25, 25 ms | 6 ms |
| A 300 dpi picture of a plot | 152 ms | 255, 248 ms | 146 ms find, 53 ms trace |
| A scan turned 2.4° | 225 ms | 348, 346 ms | 227 ms find (incl. straightening), 64 ms trace |

So a plot is digitized in one call, in under half a second. On a vector page most of that time is the overlay PNG (85 to
140 kB). Each answer gives its own breakdown (`ms`: find, calibrate, extract, all). The tests assert nothing about time,
because timing bounds have gone red on slow runners before (3972088, c6c7eb2). These figures come from one run on the
machine above.

## Claude's tools

- **`find_plots {path, pages}`:** for each plot, its page, box, route, and axes (scale, from, to, title, the ticks used,
  labels left out). It also says whether the plot is calibrated, or lists the tick marks a scan needs values for. It looks
  at pages for up to 20 s and lists the pages it did not reach.
- **`digitize_plot`:**
  - **Choosing the plot:** `page` and `plot`, or `region`. Without `page`, the first plot from the page in view. On a page
    with several plots and no `plot` given, it lists them and stops.
  - **Correcting or supplying the calibration:** `x_scale` and `y_scale`; `x_values` and `y_values`; `references`, with
    `polar` and `r_at_centre`, and `model: "projective"`.
  - **Choosing what is taken:** `colours`, `seeds` (one series per point on a curve), `route`, `trace_scale`, and `names`
    to rename the series.
  - **Output size:** `max_points` per series, 200 by default, thinned with Douglas–Peucker so the shape is kept; 0 returns
    every point. `picture` (on by default) and the usual picture arguments control the overlay.
  - **Writing out:** `csv` (`series,x,y`) and `dataset`. The dataset is a Qucs `.dat` in which each series depends on its
    own `<name>_x`, which `add_diagram`/`add_trace` can plot beside a simulation. A test reads it back with the program's
    own dataset reader.
  - **Keeping:** `keep` (with `replace`) stores the plot with the PDF, and an open tab shows it at once. `show` also opens
    the panel on it.
  - **Errors:** bad input is refused with the reason. Wrong value counts say where the marks are. A kept file that cannot be
    read is left untouched.
- **`get_digitized {path, plot, max_points, picture}`:** what is kept, in the axes' units. If the PDF has changed since,
  it says so.
- **Wiring:** all three are in the schema, the summaries, the search keywords, the auto-mode list, the tools allowed while a
  dialog waits, and the dispatch. `find_plots` and `get_digitized` are read-only and `digitize_plot` is additive.
  `digitize_plot` joins `read_pdf` and `show_pdf` in the tools sent every turn while a PDF is open, and the instructions
  tell Claude to use it rather than reading values off a picture by eye.

## The panel

`plotdigitizerpanel.{h,cpp}`. The viewer gains `PageTool`, an interface for something that lies over the pages and takes
the mouse (`pdfdoc.h`):
- The panel draws into a transparent picture of the part of the page in sight. Either of the view's surfaces, CPU or GPU,
  can show that picture.
- With a mode on, the left button is the tool's. The middle button still pans.
- Escape and Ctrl+Z are taken on `ShortcutOverride` while the tool wants them, as the pens' keys are.

Panel details:
- **Overlay:** each series is drawn in the colour complementary to its own, with its points marked. A trace that has
  wandered off its curve stands out.
- **Magnifier:** a 5× lens by the pointer, with the marks drawn in it and the value under the pointer.
- **Undo:** keeps up to 64 states.
- **Saving:** at every change, with `QSaveFile`. If the folder is not writable, the file goes under the application's data
  folder, named from the PDF's path.
- **When the PDF changes:** reading the file again keeps the plots. A kept file whose PDF has changed is still shown, with
  a warning.

**On the brief's "saved with the PDF as the pen strokes are":** the pens' strokes are not saved at all. They belong to the
tab (see 2026-11-02-pen-markers.md). The digitizer keeps its own file beside the PDF, which is what the brief asked for.

## Tests

- **`test_plot_digitizer`:** the engine alone, about 1,700 lines, 72 cases. It links only the engine and Qt Core, Gui, Pdf
  and Test, so it builds in seconds, under the sanitizers too. It covers:
  - Calibration: two axes, log and reversed, turned and sheared, projective, polar, and each way a calibration can fail.
  - Twenty-seven forms of tick label, both ones that should read as numbers and ones that should not.
  - Every row of the accuracy table above.
  - Bars, a second axis, a stray label among the ticks, and colour selection.
  - The reader against pdfium on seven page boxes: plain, turned 90, 180 and 270, cropped, cropped and turned, and
    `/UserUnit`.
  - Hand-written PDFs: crop boxes, rotations, an encrypted trailer, a page tree and a form that refer to themselves, a form
    used as a marker, ASCIIHex.
  - Damaged files: an empty file, a non-PDF, a file with no cross-reference, one with wrong offsets, and a file cut short at
    six places.
  - Fuzzing:
    - The reader: 1,500 damaged files; 273 of them still open.
    - Content streams: 400 random ones.
    - The kept file: 4,000 damaged ones; 281 still read. Everything read is then used.
  - CSV quoting (including a name that would be a spreadsheet formula), dataset names, thinning, and saving and restoring,
    including a stale or damaged kept file.
- **`test_plot_digitizer_app`:** in the program, 24 cases:
  - Every tool, with its refusals.
  - A scan given its tick values.
  - References and seeds.
  - CSV and a dataset, read back by `qucs_s::dataset::Dataset`.
  - Keeping, replacing, and a damaged kept file left alone.
  - `show`.
  - Over stdio against the program with no window.
  - The panel driven by the mouse on the pages: its buttons, every mode, Escape and Ctrl+Z through the window's shortcuts,
    the overlay and the magnifier changing the view's pixels, and restoring on reopening.
  - `QUCS_DIGITIZER_SHOTS=<folder>` writes the pictures in this folder.

**Results:**
- **Release:** all pass:
  - the two new tests;
  - the affected suites: `test_pdf_viewer`, `test_qucs_control`, `test_mcp_server`, `test_mcp_wishlist`,
    `test_claude_fuzz`.
- **Earlier failures, and why:**
  - `test_qucs_control` first failed because the new tools' action phrases lacked "in Qucs-S". That is now fixed.
  - Its `aLargeSchematicIsArrangedInTime` failed once while the sanitizer tree was building beside it, and passes on an idle
    machine.
  - `test_mcp_wishlist` needs `qucs-strans` and `rxcalc` built. A build of selected targets lacks them, and that test then
    fails for that reason alone.
- **ASan/UBSan** (Debug, `-fsanitize=address,undefined`): all pass with no report from either sanitizer: the two new tests
  (rerun on the final code), `test_pdf_viewer`, `test_qucs_control`, `test_mcp_server`, `test_mcp_wishlist` (with
  `qucs-strans` and `rxcalc` built) and `test_claude_fuzz`.
- **The four lints:** `check-tool-arguments.py` (0 disagreements), `check-third-party.py`, `check-json-nesting.py` and
  `check-shaders.py` are clean. `check-tool-arguments.py` gained one line, telling it which function reads
  `digitize_plot`'s `references` items.

## Files

- **New:**
  - `pdfvector.{h,cpp}`, `plotdigitizer.{h,cpp}`, `plotdigitizer_p.h`, `plotdigitizer_trace.cpp`,
    `plotdigitizerpanel.{h,cpp}`, `qucscontrol_digitizer.cpp`
  - the two tests
  - this note and its folder
- **Shared, with small additive changes:**
  - `pdfdoc.{h,cpp}`: the `PageTool` hook, the Digitize button, the panel's lifetime.
  - `qucscontrol.{cpp,h}`: the three tools in each list, three declarations, one sentence of the instructions.
  - The two `CMakeLists.txt`.
  - `check-tool-arguments.py`.
- **Not touched:** `qucscontrol_fem.cpp` and the multiphysics files.

## What could not be verified

- **Qt 6.10.3.** Built and run against Homebrew's Qt 6.12.0 only. The code uses `QPdfDocument`, `QPdfSelection` and
  `QPdfDocumentRenderOptions` (scaled size and clip rectangle), all of which 6.10 has, and no API newer than that as far as
  I know. CI has not built it.
- **The real display, by hand.** The app-level test ran on the real display (`-platform cocoa`, Retina, dark theme), driven
  by QTest's synthesized events: 23 of 24 pass, and the picture at the top comes from that run. The overlay lines up at a
  pixel ratio of 2. The one failure is the magnifier test: on macOS QTest's synthesized pointer move does not reach the
  window, so no lens was drawn. The same test passes offscreen. Nobody used the panel with a real mouse.
- **The GPU view.** The overlay goes through the `Surface` interface both views share, but it was not tested with
  Viewers/pdfGpu on.
- **Papers and datasheets from the wild.** Apart from the repository's Qucs plot and the matplotlib files, no third-party
  PDF was tried: there are none in the repository, and none were downloaded. Lumerical's digitized figures in
  2026-10-20-claude-coupled-exciton-strength.md were done outside the program. Redoing them this way would be the first real
  check.
- **Encrypted and Type 3 heavy PDFs.** Encrypted files go to the raster route, and that is tested only by an unencrypted
  file with an `/Encrypt` entry added. Plots drawn with Type 3 glyphs as markers are read like any other form.

## Considered, not done

- **Linking pdfium directly** for its paths. It is inside QtPdf, but its symbols are hidden. Building it separately would be
  a large third-party dependency, so I chose the small reader instead.
- **An OCR engine** for scanned labels. See above: the user's decision.
- **Calibrating polar plots and detecting frameless axes automatically.** Both are possible from the vector art, but each is
  another detector to get wrong. References cover both today.
