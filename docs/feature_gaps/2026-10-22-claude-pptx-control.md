# Claude drives the presentation viewer

The user (2026-10-22), after [the viewer](2026-10-22-claude-pptx-viewer.md) was committed (d37ee17):
*"now add mcp support for the built-in pptx viewer. make sure claude inside qucs has full control over the
built-in pptx viewer"*.

Before this, Claude had `read_pptx` (the slides' text and notes, pictures of them) and `show_pptx` (a
slide by number or title, a search's first hit, a zoom, the notes pane, a slide show started or ended).
The rest of the tab could only be reached through the generic tools, and part of it not at all.

## Everything the tab does, and how Claude does it

| In the tab | Claude |
| --- | --- |
| The slide before, after, first, last; a number typed | `show_pptx` `slide`: next, previous, first, last, a number, a title or its start |
| The find bar: typed, Return, Shift+Return, closed | `show_pptx` `find` (the bar open with it, the first hit from the slide shown on, as typing it), `find_next` next or previous, `find` "" closes it |
| Zoom out, in, the zoom box, Ctrl+wheel, a pinch | `show_pptx` `zoom`: a percentage, fit width, fit slide |
| Looking closely at a part of a slide | `show_pptx` `region` (new): that part as large as the tab takes it |
| The notes pane, the thumbnails | `show_pptx` `notes`, `sidebar` (new) |
| Play, F5, Shift+F5, a double click | `show_pptx` `play`: true (from the slide shown), "first" (new), false |
| In the show: on, back, Home, End, a number, Escape | `show_pptx` `slide` while a show plays moves the show (new), past the last its black end, a step more ends it |
| ⋯ > Reload | `show_pptx` `reload` (new) |
| Copy the slide as a picture, its text | `show_pptx` `copy`: picture or text (new) |
| Export the slide as a picture | `export_image` of a presentation (new): any slides as png, jpeg, bmp, tiff or webp (several numbered after the name), or one pdf of a slide a page; the presentation open or not |
| Print | `export_image` to a pdf; or File > Print through `trigger_action` and the dialog's tools |
| What the tab shows | every `show_pptx` answer, and `read_pptx`'s `its tab` (new): the slide and its title, hidden or not, the zoom, the panes, the find bar and its hits, the show playing and its slide or end |
| Looking at a slide | `screenshot` `slide` and `region` (new): drawn from the file, the tab left as it is; `read_pptx` `image` with `region` (new) |
| The bar's controls | `get_ui`/`set_ui` area `document`: the Slide field, the Zoom box, the Slides thumbnails, the buttons by their tooltips |
| The ⋯ menu | `set_ui` `press` "More > Reload" (new, any menu button) |
| A right-click on a slide | `context_menu` `{"slide": n}` (new) |
| Open, at a slide | `open_document` `page` or `find` of a presentation (new); `show_pptx` opens one too |
| Save As (a copy), Close | `save_document` `as`, `close_document` (as before) |

`show_pptx` does what it is asked in a fixed order (the file read again, the slide, the find bar, the
zoom and region, the panes, the show, the copy). It checks every argument first, so a wrong one does
nothing at all: a slide asked with a zoom of "huge" leaves the tab where it was.

A slide show is a window of its own over the screen. Claude steps it with `slide` as the user steps it
with the keys: hidden slides are left out, "next" past the last slide shown goes to the black end and
again ends the show, which leaves the tab at the last slide shown.

## Found and fixed

- **M1.** `set_ui` pressing a button that opens a menu (the ⋯ of the presentation's bar) never returned.
  The menu runs a loop of its own that waits for a click, and the call's answer waited behind it: in
  the test, a minute later it had not returned. Over the server, at best the menu would stay open over
  the window until the user closed it (not tried).
  This holds for any such button in a document's bar or a dialog, not only this one. Now such a button
  is not pressed: its entries are said, and `press` "Button > entry" chooses one as the menu would.
  `get_ui` lists a button's menu with it. Found by the first probe of the generic tools on the tab.
- **M2.** The bar's controls were badly named for `get_ui`. The zoom box took the "of 5" beside it, the
  slide field its long tooltip, and the thumbnails their object's name ("pptxThumbnails"). They are now
  named Zoom, Slide and Slides, by labels of their own and accessible names. The find bar's close button
  was "✕"; it now draws a glyph and is named by its tooltip.
- **M3.** `list_documents` left presentations out of the workspace's files. They are now listed as the
  kind "presentation".
- **M4.** The right-click menu lacked half of the ⋯ menu: Copy Path, Play from the First Slide, Show in
  File Manager and Reload. Both menus are now filled by one function, with the fits and Play from This
  Slide too.
- **M5.** Closing the find bar moved the focus to the slides wherever it was before. Closed by Claude
  while the user typed in the Claude panel, it would have taken the user's typing away. It now moves the
  focus only from the find field.
- **M6.** `slideShow()` still returned a show that had just closed, until it was deleted a moment later,
  so a state read right after a show ended said it played. It now returns only one that is shown.

## Permissions

Nothing new is pre-allowed or asked about: `show_pptx` only shows, copies to the clipboard or reads its
file again, and stays allowed in auto mode. `read_pptx` and `screenshot` only read. `export_image` writes
files, and is asked about as before when it would write over a file. For slides, each numbered picture
is checked, not only the name given. The files guard sees each numbered picture that would be written
outside the workspace, and the deck read when it is outside. Opening the system's viewer or the file
manager is reached only through `set_ui` and `context_menu`, which drive the window and are asked
about in auto mode.

## Not done

- A PDF or a picture of a slide holds a picture of it: its text is not selectable. The engine draws a
  slide through an image (for printing too). A vector export (text as text) would mean painting onto
  the PDF device directly, with the engine's 72-dpi text layout; it is not tried here. SVG is refused
  for the same reason: it would only wrap a picture.
- The slide show's own window is not among `screenshot`'s pictures (the `window` area is the main
  window and its dialogs). `screenshot` `slide` draws the slide it shows.
- Export the Slide as a Picture… still opens its file dialog. Claude uses `export_image`, or goes
  through the dialog with `get_dialog` and `set_dialog`.

## Tests

`test_pptx_doc` `claudeDrivesTheTab` (new) drives one deck (five slides, the fifth hidden, a red box on
slide 2) through all of the above. Among its checks:
- each step and a title;
- a wrong argument doing nothing;
- the find bar's three hits in turn and round again;
- a region filling the view with the red box, the panes, a reload, both copies;
- a show stepped to its last shown slide, its first, its end and out;
- `read_pptx`'s tab state and a region's picture, `screenshot` of a slide's region in fractions;
- `get_ui`'s names and the ⋯ menu's entries, `set_ui` on the field, the zoom box and a thumbnail;
- the ⋯ pressed (answered within 5 s, no menu left open) and an entry chosen;
- `context_menu` on a slide;
- `export_image` of the slide shown, two numbered, all to a PDF (five pages, the hidden one said),
  greys, a deck not open, svg refused, and the questions it asks;
- `open_document` at a slide and at a find;
- `list_documents`.

`claudesTools` reads the show's new state.

The suites that use the tools touched pass:
- `test_pptx_doc` (19, the two on request skipped; under AddressSanitizer and UndefinedBehaviorSanitizer
  too, with no report);
- `test_qucs_control` (209), `test_mcp_server` (18), `test_claude_code` (68, 1 skipped);
- `test_pdf_viewer` (28, 3 skipped), `test_layout_doc` (30, 6 skipped), `test_multiphysics_doc` (82);
- `test_diagram_tools` (41), `test_component_symbols` (13), `test_git_integration` (31),
  `test_wishlist_tools` (23), `test_claude_fuzz` (3).

So do CI's checks of the tools' JSON and arguments and of vendor names.

There were 42 breaks, each its own edit, built and run, then put back, and all were caught. Before the
run, two checks were sharpened:
- the find bar's text was put on a third slide: with two hits, "next" and "previous" land alike;
- the show is now stepped to its last and first slides too.

The break that presses a menu button again (M1) was caught by the 5-second limit on the answer.
