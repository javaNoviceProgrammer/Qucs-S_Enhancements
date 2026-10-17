# The Markdown preview in a dark theme

Found while adding GPU drawing to the viewers (80aeefd), and flagged as a task of its own, done here.

**The fault.** In macOS dark mode with the platform theme (no designed theme), the Markdown preview showed black text on a near-black background (#171717 on the display, #232323 offscreen).

**Why.**
- Under the platform themes the preview, like the text editor, is meant to be black on white, light or dark (`TextDoc::paperAndInk`). The rendered document carries that ink in its formats.
- `renderNow` gave the preview its paper and ink as a palette.
- The main window has a style sheet. Qt's style-sheet style gives each widget it polishes an explicit copy of its palette at that moment.
- The preview's viewport was polished before the preview's palette was set, so it kept the application's palette: in a dark theme, the dark base under the black ink.
- The text editor had met this before and sets its colours as a style sheet. Its comment says why.

**Two tests showed it, and failed on the display only:**
- `quotesHaveTheirBar` (dark mode);
- `drawnWhereLaidOutInAWindow`, which also read its pixels without the picture's pixel ratio, so it read the wrong pixel on a Retina screen.

The suite runs offscreen, with no dark mode, so it never saw either.

## Now

- **The preview's paper and ink are a style sheet**, as the editor's: `QTextBrowser { background-color: paper; color: ink; }`.
  - It is set only when the colours change, because each setting polishes the preview again. Set at every rendering, it desynchronised the screen from a grab (see the breaks).
  - The palette the quote bars and the GPU's drawing read follows from it.
- **The preview follows the theme.**
  - `TextDoc::applyDocumentColors`, which the application calls on a theme change, is now virtual. `MarkdownDoc` draws its rendering again in the new paper and ink.
  - Before, a preview open across a theme change kept the old theme's colours in its document until the next edit. Under a designed theme it looked right only because the application's palette happened to be the theme's.
- **A viewport replaced by the CPU/GPU switch is polished again** (`gpu::setGlViewport`) when the area has its own style sheet. A new viewport polished only as a new widget had the application's palette, so it was dark again after a switch back to the CPU.

## Cost

Under a style sheet with a background, Qt draws the viewport's background as a styled background, not by filling it. So a scroll can no longer move the picture and paint only the lines coming in: each scroll paints the whole view. The editor has always paid this.

| A scroll's paint (median, on the display) | Before (wrong colours) | Now |
|---|---|---|
| CPU | 0.25 to 0.30 ms | 2.05 to 2.22 ms |
| GPU | 1.38 to 1.41 ms | 1.27 to 1.42 ms |
| A whole paint, CPU | 2.5 to 3.0 ms | 1.7 to 2.0 ms |
| A whole paint, GPU | 1.6 to 1.8 ms | 1.5 to 1.7 ms |

All of these are well within a frame.

**The GPU write-up's reason for the preview's CPU default no longer holds.** It said a CPU scroll was about five times cheaper, and that came from the fault: the wrong fill was what let Qt move the picture. Measured again, the GPU is now a little quicker. **The default stays the CPU**, for another reason: Qt's OpenGL engine draws glyphs a little heavier than the platform does. Pixel by pixel, 8 to 20 % of the inked pixels differ at the glyphs' edges, black on white. The tooltip and the code's comments now say so. The GPU remains a choice.

**The GPU write-up's Markdown parity figures were measured on the wrong colours.** Those were 0.28 % and 0.45 %: black on near-black hides glyph edges. Its test now compares 32 by 32 blocks' mean colours, which a glyph's weight barely moves while a missing selection or a wrong scroll moves many blocks:

| Picture | Pixels apart (glyph edges) | Blocks apart (of 1,190) | Largest block difference |
|---|---|---|---|
| Plain | 10.2 % | 0 | 9 |
| With a selection | 7.8 % | 0 | 9 |
| Scrolled | 19.6 % | 0 | 9 |

## Tests

- **test_markdown_doc `thePaperIsTheThemesNotThePlatforms` (new):** the preview's paper below a document, its ink in a line of text, and the palette its bars and selections read, through:
  - System;
  - Dark (the platform's dark palette, forced offscreen as on a platform that cannot switch);
  - Nord;
  - Daylight;
  - Dark again;
  - a file opened under Dracula;
  - on the GPU under Dracula and Dark, then back on the CPU;
  - System again.

  It fails at 80aeefd ("Dark: #232323 under the document, the paper #ffffff").
- **`theGpuDrawsAsTheCpu`:** the block comparison above; the per-pixel figure is logged.
- **`drawnWhereLaidOutInAWindow`:** reads its pixels at the picture's pixel ratio, and its comment is back above it.

Eight changes broken on purpose, all caught:

- **The fix itself:**
  - the colours a palette again (caught offscreen);
  - no rendering again on a theme change (caught at Nord);
  - the sheet without its ink (caught at Dark: the text the application's light ink on the white paper);
  - a new viewport not polished again (caught on the display, after the switch back to the CPU);
  - the sheet set at every rendering (caught: the screen's frame and the grab 72,876 pixels apart).
- **The earlier GPU breaks, against the block comparison:**
  - the preview without its selection (127 blocks apart);
  - not scrolled (519 blocks apart);
  - without the quotes' bars (their exact pixels).

## Runs

- **The Release suite (offscreen):** 127 of 127, after the rebuild that followed the breaks.
- **On the display:**
  - test_markdown_doc 18 of 18, the fuzzer skipped; both earlier failures pass now;
  - test_app_theme 19 of 19;
  - the PDF and layout GPU functions pass.
- **Under the sanitizers,** offscreen and on the display: test_markdown_doc and test_app_theme. No report.
- **The lints pass.**

**Still failing on the display, as at 5e4a02f:** four layout picture tests, and the PDF every-command test that can stop in macOS's save panel. Neither involves the preview.
