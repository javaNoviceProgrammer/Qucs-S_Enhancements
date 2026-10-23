# A PowerPoint viewer in a tab

The user (2026-10-22): *"Let's add a native pptx viewer to the qucs"*.

A presentation (`.pptx`, `.pptm`, `.ppsx`, `.ppsm`, `.potx`, `.potm`) now opens in a tab of its own,
like a PDF. It is read and drawn by Qucs-S itself (`pptx.cpp`): there is no Office, LibreOffice or Quick
Look behind it, so it works the same on macOS, Linux and Windows. Macros in a `.pptm` are never run. It
only views a presentation: nothing in it is edited.

It opens from File > Open (a "Presentations" filter), the project view, a file dropped on the window, and
Claude's tools.

## The tab

- **The slides**, one under the other, scrolled through continuously. Each one is drawn when it comes
  into sight and kept for the zoom it was drawn at. The slide in sight is the one at a third of the
  view's height.
- **The toolbar**: the thumbnails' sidebar; the slide before and after; a field with the slide in sight
  (type a number, press Return) and "of N"; zoom out, a zoom box (Fit Width, Fit Slide, 10 % to 800 %,
  or typed) and zoom in; the notes pane; Slide Show; Find; and a ⋯ menu.
- **The thumbnails**, drawn a few at a time after the tab opens, so a long deck opens at once. A hidden
  slide's thumbnail says "(hidden)".
- **The speaker notes** of the slide in sight, in a pane below, with "No speaker notes." where there are
  none. The pane also says what of the slide is not drawn as PowerPoint draws it (see below). Whether
  the notes and the sidebar are shown is kept in the settings (`Viewers/pptxNotes`,
  `Viewers/pptxSidebar`).
- **Find** (Edit > Find, Ctrl+F): the slides' text and notes, case ignored. Return goes to the next
  slide with it, Shift+Return to the one before; the label says "Slide 3: 2 of 5 slides with it", or
  "Not found".
- **Slide Show**: full screen on black, from the slide in sight. F5 plays from the first slide and
  Shift+F5 from the one in sight, as in PowerPoint, while the focus is in the tab; elsewhere F5 is still
  Show Last Messages. Hidden slides are left out. On with a click, Space, an arrow or Page Down; back
  with Backspace, the left arrow or Page Up; Home and End; a number and Return goes to that slide;
  Escape ends it. The tab then shows the slide the show ended at.
- **Zoom**: View's zoom actions, Ctrl+wheel and a pinch. Page Up/Down, Space, Home and End move by
  slides.
- **The ⋯ menu and right-click**: copy the slide as a picture (Edit > Copy, Ctrl+C, too: 1920 pixels
  wide), copy its text with its notes, export it as a picture, play from this slide or the first,
  reload, copy the path, open with the system's viewer, show in the file manager.
- **Changed on disk**: the file is watched and read again when it is written, keeping the slide in sight
  and the zoom. A file deleted to be written again (a deck made anew) is shown as it was meanwhile.
- **Save As** writes a copy of the file. **Print** puts a slide on each page, landscape for a wide
  slide.
- Simulate on a presentation says "A presentation is not simulated: Play shows its slides."

A file that cannot be read says why in the tab: not a ZIP archive, no presentation in it, or "empty (a
file kept online, not downloaded?)". The last is what a cloud folder's placeholder of a file not
downloaded looks like; 741 of the 1011 decks found on this machine were such Dropbox placeholders.

## What is drawn

The slides are read from the package (Office Open XML, ECMA-376): the slide, its layout, the layout's
master and the master's theme. What a slide does not say itself it takes from the layout's and the
master's placeholders of its kind (by index, then by type): where a title or a body is, its text's
styles at each level (the master's `txStyles`). The master's and the layout's own shapes are drawn under
the slide's, unless the slide or the layout says not (`showMasterSp`).

- **Colours**: the theme's, through the master's colour map and a slide's or layout's override; the
  modifiers (`lumMod`, `lumOff`, `tint`, `shade`, `satMod`, `alpha` and others). The theme's line, fill
  and font styles that a shape refers to (`lnRef`, `fillRef`, `fontRef`), with the placeholder colour.
- **Backgrounds**: the slide's, the layout's or the master's; solid, gradient or picture.
- **Shapes**: 78 of the preset outlines (rectangles, rounded and snipped, ellipses, triangles, arrows,
  callouts, braces, stars, flowchart shapes, the connectors and others) with their adjustments; custom
  geometry; rotation and flips. A shape of another preset kind is drawn as its rectangle, and the notes
  pane says so.
- **Fills and lines**: solid, linear and radial gradients, pictures (stretched or tiled), patterns (as
  the mix of their two colours); line widths, dashes, joins and arrowheads.
- **Groups**, at their own size and position (their child space mapped onto them).
- **Pictures**: PNG, JPEG, GIF, BMP, TIFF and SVG, cropped as the slide says. A video's or a sound's
  poster picture is drawn, but they are not played.
- **Text**: fonts (the theme's, with substitutes where a font is not installed), sizes, bold, italic,
  underline, strike-through, colours, super- and subscript, highlights, alignment, spacing, insets,
  anchoring (top, middle, bottom), vertical text, autofit's font scale. Bullets hang as in PowerPoint:
  characters (Wingdings' mapped to Unicode); numbers, Roman numerals and letters with their stops
  and brackets, counted on across a deeper level's paragraphs (other schemes, circled or East Asian
  numbers, as plain numbers).
  A slide number field shows the slide's number; other fields (a date) show what was last saved.
- **Tables**: cells' fills, borders, margins and anchors, merged cells, and the built-in table styles'
  header row, banded rows, first column and last row.
- **Charts**, from the values saved with them: bar and column (clustered, stacked, percent), line,
  scatter, area, pie and doughnut; titles, legends, axes (logarithmic too), gridlines, markers, smoothed
  lines. Other kinds are named in the notes pane.
- **SmartArt**, through the drawing PowerPoint saves with it. **Equations** and other new content,
  through the picture a file keeps for older readers (`mc:AlternateContent`'s Fallback).
- **Embedded objects** (an equation editor's, a spreadsheet), through their picture.

Not drawn, and named in the notes pane and by `read_pptx`:
- EMF and WMF pictures (a box in their place). These are Windows metafiles, which Qt cannot draw. They
  are common in older decks made on Windows: of the 4587 slides of the decks on this machine, 159 have
  an EMF picture and 3 a WMF one.
- A SmartArt diagram saved without its drawing; an object without a picture; a picture linked from
  outside the file.
- Preset shapes beyond the 78, which are drawn as rectangles: 4 slides here (`curvedLeftArrow`,
  `leftUpArrow`).

Never drawn, and not named: shadows, glow, reflections and 3-D effects; animations and transitions (a
slide is shown as it ends); comments; ink.

## Claude's tools

- **`read_pptx`** (a reading tool, so let through unasked) reads a presentation, open or not. It returns
  each slide's title, its text paragraph by paragraph (a table's rows as `cells | cells`, a chart's
  title), its speaker notes, whether it is hidden, how many pictures, tables and charts it holds, and
  what of it is not drawn. `slides` picks some (`[3, 4]`, `"2-5"`, `7`); `search` returns the
  paragraphs and notes that have a word or value; `image` draws up to four slides as pictures. The
  answer is cut at 40,000 characters, and what is left out is named.
- **`show_pptx`** shows a presentation in its tab, opened if it is not: a slide by number or by title
  (or the title's start), a search, a zoom, the notes pane, or a slide show (`play`). It returns the
  slide shown, its title, the zoom and the hits. It only shows, so auto mode lets it through with the
  other tools that show.

A presentation outside the workspace is asked about by the guard of files outside, as any other file
is.

While a presentation is open, both tools are in every turn's tool list, and `get_state` names them.
`list_documents` gives an open one the kind "presentation". The server's instructions name them beside
`read_pdf`.

## Found while testing

- **K1.** The tab's own keys did nothing. Ctrl+F, F5 and the menu's Ctrl+C were actions of the tab, and
  the window has actions of the same keys (Edit > Find, Show Last Messages, Edit > Copy). Qt then takes
  neither ("Ambiguous shortcut overload") and does nothing. Found by pressing the keys in the test,
  where the first version only called the slots. Now Edit > Find and Edit > Copy do it for a
  presentation; the menu's Copy shows its keys but takes them only while the menu is open; and the tab
  claims F5 and Shift+F5 before the shortcuts are looked at (`ShortcutOverride`), while the focus is in
  it.
- **K2.** A chart's text size or marker size from the file was not bounded: a `sz="0"` would ask Qt for
  a font of 0 points. Sizes are now held to the schema's range (1 to 4000 points of text, 2 to 72 of
  marker); a run's text size alike.

## Not done

- **EMF and WMF pictures**. Drawing them means a metafile player (a few thousand lines for EMF's
  records), or converting them with a tool where one is installed (LibreOffice). They are the largest
  gap on the decks here.
- Shadows and other effects; the remaining preset shapes; charts of the other kinds (radar, bubble,
  stock, surface, the 2016 kinds).
- A preview of a deck's first slide in the File Browser.
- Fonts: a deck's embedded fonts are not used. A font that is not installed is replaced by a near one
  (Calibri by Carlito, for example, where it is installed), so a line may break elsewhere than in
  PowerPoint.

## Tests

`test_pptx_doc` builds its decks in the test (a theme, a master with its text styles, a layout, slides,
notes and other parts), so no file is checked in. Its functions:
- `presentationFiles`, `notPresentationsSaid` (an empty file, not a ZIP, a workbook, a deck without
  slides);
- `slidesTextAndNotesRead` (titles, paragraphs with their bullets, notes, hidden slides, find);
- `shapesDrawn` (fills, theme colours and modifiers, the theme's styles, lines, flips, a hidden shape, a
  shape of an unknown kind), `groupsDrawn`, `picturesDrawn` (crops, a picture fill, an EMF);
- `textFormatted` (bullets and numbering by level, a bullet hanging an inch before its text, anchors,
  autofit);
- `placeholdersFollowTheirIndex` (of a layout's two content placeholders, the one of the slide's
  index);
- `tablesDrawn` (the header, banded and plain rows, a merged cell), `chartsDrawn` (bars upright and
  across, their lengths, lines on a log axis, a scatter, a pie);
- `backgroundsAndColourMaps`, `mastersShapesDrawn`, `alternateContentsFallback`, `smartArtDrawn`;
- `opensInATab` (the real window: thumbnails, the slide field, notes, zoom, find, copy, reading the file
  again, Save As, printing to PDF, the slide show; Ctrl+F, Ctrl+C, F5 and Shift+F5 pressed in the tab);
- `claudesTools` (`read_pptx` and `show_pptx`, their errors, a slide found by its title's start, the
  lists the tools are in, a file outside the workspace asked about).

Two more run only when asked, on real decks: `showDeckGiven` (a window grab of a deck's tab) and
`renderDecksGiven` (every slide of the decks named in `QUCS_PPTX_RENDER`, timed).

The suites of the programs touched pass: `test_pptx_doc` (18, the two on request skipped; under
AddressSanitizer and UndefinedBehaviorSanitizer too), `test_qucs_control` (209), `test_mcp_server` (18),
`test_claude_code` (68, 1 skipped). So do CI's checks of the tools' JSON and arguments and of vendor
names.

The decks on this machine: of 1011 files found, 741 were cloud placeholders (empty) and were not
downloaded. The other 270 decks, 4587 slides, were read and drawn under AddressSanitizer and
UndefinedBehaviorSanitizer, with no report. Some of the user's own decks (the APD, Liu 2013 and Bahadori
decks, the Pozar and Yariv chapter decks) were drawn and looked at slide by slide while the engine was
written; the others were only read and drawn, not looked at. In Release, a deck is read in 22 ms (the
median; 0.42 s the slowest, 58 slides), pictures decoded when first drawn, and a slide is drawn in
2.2 ms on average at a point a pixel.

There were 43 breaks, each its own edit, built and run, then put back. All were caught, five of them only
once the tests were sharpened:
- placeholders matched by type alone: the test's layout had one body placeholder, which type finds too
  (`placeholdersFollowTheirIndex` is new);
- a bullet set before its text as a character, not hanging: at the master's 18-point indent the text
  starts within a point of where it hangs (a bullet an inch before its text now);
- a table's banded rows not drawn: the band was checked as light blue, which the whole table's tint is
  too (now against a plain row);
- a bar chart's bars drawn upright: the test had only a column chart (now one of each, their lengths
  measured);
- a slide found by its whole title only: "Back" was in "Backup" anyway (slide 1 is now "Introduction and
  background").

The four breaks of the keys (K1) were caught by the keys pressed in `opensInATab`.
