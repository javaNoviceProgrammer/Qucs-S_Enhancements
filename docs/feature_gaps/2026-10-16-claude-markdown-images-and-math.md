# Markdown's images in tables and drawn smoothly; math sharp, and found in more forms

The request (2026-10-16): "Looks like image rendering and resizing in markdown still has some
bugs. If an image is inside a table, it won't properly resize. If an image resize, it's quality
drops a lot. Are we doing anti-aliasing for rendering images in the markdown viewer? Looks like
markdown viewer doesn't render inline latex equations. please check and fix it. Also, I feel like
the image quality of rendering latext equations in the markdown viewer and claude chat window is
not great. can you check that, too?"

## What was wrong

- **An image in a table** was fitted to the whole rendering's width (be12afb fitted each image to
  its paragraph's room, and a table's cell was taken for the page). Qt's layout keeps a table to
  the page and squeezes its columns, so an image wider than its column ran over the cell beside
  it and past the edge. As the tab narrowed, the image did not shrink with its column.
- **No smoothing.** Qt's image handler (QTextImageHandler) draws an image's own pixels into its
  rectangle with the painter as it is, and QTextEdit's painter has no smooth transform: each
  device pixel took the nearest pixel of the picture. A plot fitted to a third of its width kept
  one column of pixels in three, so its lines and letters broke up. On a Retina screen an image
  at its own size was doubled pixel by pixel.
- **The viewer's math was typeset at one pixel to a point**, and so scaled up and blurred on a
  Retina screen. The chat typeset at the screen's ratio already. **Both drew it at a fraction of
  a pixel** (where its line put it), with a smooth transform, so every pixel was blended with its
  neighbours.
- **Inline math.** I could not make it fail. The user's own MQW_modulator_open_gaps.md (a copy,
  17 formulas), the photodetector review and sixty forms (in tables, lists, quotes, headings,
  bold, links; `\frac`, `\text`, `\operatorname`, `\left( \right)`, `\mathbb`, `pmatrix`,
  `\sqrt[3]`, GitHub's ``$`...`$``) all render in the viewer, each typeset. What did not render
  were other ways of writing math: GitHub's display block (a fenced block of the language
  `math`), a bare `\begin{equation}` or `align` environment as MathJax takes it, and `\\(...\\)`
  (written so that Markdown's escape leaves `\(`). These were shown as code or text. `$ x $` with
  spaces inside stays text, as in pandoc and GitHub. Blurred at 1x on a 2x screen, the viewer's
  inline math may also have looked like plain text.

## What it is now

**Images in a table fit their cells** (markdowndoc.cpp, fitImages): a cell's room comes from its
table. The table's margins, borders, padding and spacing come off the room it has, and the rest
is shared among the columns. A column with no image gets at most its text's width set in a line,
which is all Qt's layout gives it. The columns with images share what is left equally (a spanned
cell gets its columns' widths). Nested tables and frames (the chat's prompt bubble) are taken
the same way. Beside "a few words" a plot takes 947 of 1059 pixels. Two side by side take 508
each. Beside a long note it takes half. At every width each image ends before the next cell.

**Images are drawn smoothly** (qucs_s::markdown::SmoothImages, in the viewer and the chat): the
document's image handler is replaced. Each image is scaled to the device pixels it covers, on
whole pixels. Scaling down averages every pixel it takes in (Qt::SmoothTransformation, a box
filter); scaling up interpolates between pixels. The scaled image is cached (96 MB) until its
size changes. An image at its own size is copied pixel for pixel. Its size and a missing image
are Qt's (its handler asked). Printing or a PDF is left to Qt, so the PDF keeps the image's own
pixels.

**Math is sharp**: the viewer typesets at its screen's ratio and again when the window moves to
another screen (Qt 6.6's DevicePixelRatioChange). MathObject draws a formula on whole device
pixels, a pixel of it to each, at its place under the painter's own transform.

**Math found in more forms** (mathtypeset.cpp, findMath, for the viewer and the chat alike):
````
```math            ~~~math           \begin{equation}     \\( a^2 \\)
E = mc^2           x                 E = mc^2              \\[ b \\]
```                ~~~               \end{equation}
````
A math fence is display math, also in a list item, its indent taken off. It is not math inside
another code block, or when a line starts with inline code (```` ```a``` ````). The bare
environments recognised are equation, align, gather, multline, eqnarray and displaymath, with
or without their stars. The typesetter already knew them inside dollars. `itemize` and the like
are not math.

Found on the way: the first version of SmoothImages and of the math drawing reset the painter's
transform to draw in device pixels. A widget is painted into its window with its place in the
window applied by the painter, so the reset moved it twice: the chat's pictures were drawn about
65 pixels below its prompt's frame, over the reply. A plain document drawn into an image hid this.
Both now draw under the painter's own transform, and a test draws them in a window.

## Checked

test_markdown_doc:
- imagesInATableFitTheirCells (new): three tables in the viewer at 1400, 900, 700, 450 and 1400
  pixels. Each image ends before the next cell or the margin, keeps its proportions, and there is
  no horizontal scroll bar. The image beside a few words gets more than 60 % of the room, two
  images are equal, and one beside a long note gets more than 30 %.
- imagesAreDrawnSmoothly (new): 1-pixel stripes fitted to a third. Qt's handler draws under 5 %
  grey pixels across them, ours over 95 % (over 90 % at 2x). A 60 by 20 image is drawn pixel for
  pixel at a fraction of a pixel down. Drawing to a QPicture is Qt's.
- drawnWhereLaidOutInAWindow (new): a browser placed at (30, 70) in a window, the window grabbed.
  The image is green at its middle and the formula has ink in its place in its line.
- mathIsSharpAndFound (new): a formula typeset at 2x, drawn at (10.3, 5.7), equals its own pixels
  at the nearest whole one. findMath on every form above, and on what is not math. The forms are
  rendered, each typeset with nothing of its fence left, and a display block is centred.
- imagesFitTheRendering: the viewer's images drawn by SmoothImages.

test_claude_code mathIsTypeset: a reply's math block is typeset, and the transcript's images are
drawn by SmoothImages.

Fourteen changes broken on purpose. Twelve were caught at once: the text columns not capped, the
viewer without SmoothImages, scaling by the nearest pixel, the image's and the math's transform
reset (moved twice), math not snapped, render() at 1x, no math fences, no bare environments, no
`\\(`, a backtick line taken as a fence, the chat without
SmoothImages. One was caught after its test was made to see it: **a table's image fitted to the
whole width**. The test compared each image with its block's width, and a block is as wide as
its image. Qt keeps the table to the page and squeezes its columns, so the image ran over the
next cell. The test now compares where each image ends with where the next cell starts. One was
not caught: **the viewer typesetting at 1x**. The offscreen screens are 1x, so the viewer's
ratio is 1 there. render()'s own ratio is tested.

The Release suite 126 of 126. test_markdown_doc and test_claude_code under the sanitizers: no
report.

## Not done, said

- A table with a long text in it still runs a few pixels into the right margin (Qt's own layout
  gives its content the page's whole width, its borders and padding beyond). It is not past the
  viewport, and has nothing to do with images.
- `$ x $` with spaces inside the dollars is not math (pandoc's rule and GitHub's: so that
  "$5 and $10" is money).
- A list item's bullet stays at the top of a tall line of display-style math (Qt's layout).
