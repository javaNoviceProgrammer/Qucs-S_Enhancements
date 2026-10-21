# The Markdown viewer's refresh: long files, and their images

The user (2026-10-21): *"Can you check if the markdown viewer can correctly do auto-refresh and rendering
again if the md file changes on the disk? Looks like there is a bug for long markdown files with images
that doesn't do auto-refresh."*

**What was checked.** A `.md` file changed on disk is seen by the window's watcher
(`QucsApp::documentsChangedOnDisk`, 400 ms after the last change) and loaded again
(`TextDoc::reload`); `MarkdownDoc::load` renders it. That part worked, on the offscreen platform and on the
Cocoa display:
- written in place;
- written to a temporary file and renamed over it;
- written several times in a row.

The user's own report (`miller_fig3_report.md`, its plot) ten times over (2230 lines, 10 images) rendered
again 0.8 to 1.3 s after each change. But three things made a long file with images seem never to refresh.

**1. A changed image was never seen.** Only the Markdown's own file was watched. A plot written again (a
script run again, the report's text unchanged) kept its old picture until the `.md` itself changed. An
image missing at the rendering and made afterwards was worse: Qt drew it at its own size in the place of
the missing one's mark, over the image above it.

Now the preview watches its image files (`MarkdownDoc::watchImages`), and their folders, so that an image
renamed over the old one, or made after the rendering, is seen too. 400 ms after a change, each image's
size and dates are compared with those it was read with (`imagesChanged`). One that differs renders the
document again, which reads the images afresh: Qt's resources are cleared with the document. A file
beside them changed alone renders nothing.

Not watched:
- images of the web;
- data URLs;
- more than 256 image files (Linux gives a user 8192 watches in all).

**2. A long file took seconds to render, quadratically.** `qucs_s::markdown::render` typesets each formula
where its mark is. Profiling a 6000-line file (`sample`) showed two costs, each over the whole document
for each formula:
- the preview's layout was made again from the insertion point to the end (55 % of the time);
- each mark was searched for from the document's start (42 %).

3000 lines of a formula each took 4.0 s, with the window frozen; to the user the file seemed not to
refresh. Now the shading and the math are one edit (laid out once, at its end), and the marks are found
in one pass. 3000 lines take 0.37 s, and 6000 lines 0.74 s.

**3. A file written while it was read was lost.** `TextDoc::load` took the file's date *after* reading it.
A writing during the reading (a long file's text set and highlighted) was dated after, taken for the
version read, and never loaded. Now the date is taken before the reading: such a file is newer, and is
loaded again.

**Tests** (`test_markdown_doc`):
- `imagesWrittenAgainRenderAgain`. The preview shows a plot:
  - written again in place, renamed over, and made after the rendering, it is drawn anew each time
    (pixels of its colour counted in the viewport);
  - a file beside it changed alone renders nothing;
  - the web's image and the data URL are not watched.
- `longDocumentsRenderAtOnce`:
  - 1000 formulas and 10 code blocks are rendered in fewer than 20 changes of the preview's document
    (1013 before);
  - four times the length takes less than seven times the time (13 times with the marks searched for
    from the start).
- `aFileWrittenWhileReadIsReadAgain`. The file is written again from within its own loading (as the
  text read is set); the newer version is loaded.

**Breaks.** Eight breaks, all caught:
- the render's single edit;
- the one pass of the marks (caught by the timing ratio: 3.0 s against 0.23 s);
- the render as it was before;
- the images' watch;
- their folders' watch;
- the stamps' comparison;
- web images watched;
- the date taken after the reading.

The suite passes on the offscreen platform, on the Cocoa display (its GPU tests too), and under ASan.
A 60 s run of the Markdown fuzzer passes.
