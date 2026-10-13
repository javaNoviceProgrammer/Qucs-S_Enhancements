# Feature gaps: the tools can't show a page of a PDF tab

*13 October 2026 — on `7bbf088`.*

The user asked Claude to "open up the pdf and show me Fig. 2.15(c)" of a thesis (PDF page
60). `open_document` opened the PDF at page 1, and nothing the tools had could move it:

- `goto_line` refused, since the PDF is not a text document;
- `zoom` is a schematic's;
- `list_actions` had no page or find action for a PDF;
- `get_ui` did not reach the tab's own toolbar.

`read_pdf` found "Figure 2.15:" on page 60 but could not show it. The workaround was an
outside render at 300 dpi, a crop and a picture tab: a stray file in the user's project,
and the user's PDF still at page 1. The viewer itself (`PdfDoc`, `PageView`) had what was
needed - pages, search, zoom, outline - but none of it was reachable.

All four proposals are done. They were tested on a copy of that thesis through
`qucs-s --mcp-server`, with HOME, settings, trash and cache isolated:

- `show_pdf` with `"find": "Figure 2.15:"` answers page 60 (printed label 34) of 156,
  shown, the hit marked;
- `screenshot` of page 60 draws the figure at 144 dpi, the tab left where it was;
- panel (c) is the region [0.2, 0.37, 0.52, 0.58].

| Review | Now |
|---|---|
| 1. A `show_pdf` tool, like `show_layout`: open it if needed, bring it to the front, with `page` (a number, or a destination from the outline), `find` (go to the first hit, return every hit's page), `zoom` (a percentage, fit width, fit page) and `region` ([x0, y0, x1, y1] in points or fractions of the page, scrolled and zoomed into view). It returns the page shown, the page count and the hits. | **`show_pdf`, as proposed.** See the details below the table. |
| 2. `screenshot` of a PDF tab with `page` and `region`, giving the rendered page without scrolling the user's view; the same for `read_pdf` with `image: true`. | **Drawn from the file.** `screenshot` with `page` (and `region`) renders that page, or part of it, on white: a region's longer side at about 1600 pixels (1 to 8 a point), a whole page's at 2000 at most. The tab is not moved. `read_pdf` with `image` adds the pages it read as pictures (four at most), with `region` a part of each. |
| 3. `get_ui` area `document`: the tab's own toolbar as controls, so `set_ui` can type a page number or search text. | **Area `document`:** the tab in front's own bar and side - a PDF's page field, find field, zoom box and buttons, its outline and pages; a layout's, a picture's. With `set_ui` a field set there is entered, as typing it and pressing Enter does, so a page number typed into the page field shows that page. Other areas are unchanged. |
| 4. `open_document` with `page` (and `find`) for a PDF: open it at the place asked. | **Opened there:** `page` and `find` as `show_pdf` takes them. The answer says the page, the destination, and every hit's page. |

`show_pdf` in detail:

- **`page`:** a number (the page in the file); a page's label as printed (`"iv"`); or a
  destination of the outline, by its title or its start or a word of it, case aside.
  Without a match, the answer lists the outline.
- **`find`:** every page is searched at once, the search bar shows the text, and each
  hit's page is returned. The first hit is shown and marked.
- **`zoom`:** a percentage, `"fit width"` or `"fit page"`. With `find` the hit stays in
  sight.
- **`region`:** points from the page's top left, or fractions when all four are between 0
  and 1. It is zoomed to fit and scrolled to the view's middle; a page narrower than the
  view stays centred.
- **The answer:** the page shown and its label, the page count, the zoom, the hits.

The PDF reviewed, a Qt-written test PDF, and a PDF written by hand with an outline and page
labels (Qt's writer makes neither) were all used in testing.

## Tests (`test_pdf_viewer`)

- **`claudesToolsShowAPage`:**
  - by number;
  - `find` "needle": 2 hits, page 3, marked, the bar showing it;
  - nothing found said;
  - 150 % and fit width;
  - found and zoomed to 800 % at once: the hit, located by a search of the test's own, in
    sight;
  - a region as fractions: wholly in sight, as large as the tab takes it, the zoom that
    fits;
  - a region in points, its middle the view's;
  - a page or a zoom out of range refused.
- **`claudesToolsFindDestinations`:** a PDF written by hand with an outline and page labels.
  - `open_document` at "2.2.1 Modulation": page 3, its whole title said;
  - "introduction" and "Modulation", by a word;
  - the label "i", and "2" taken as the file's page 2 (label 1);
  - a title not there: the outline listed;
  - "Figure 2.15:" on pages 2 and 3;
  - opened again at a hit: brought to the front, at page 3.
- **`claudesToolsDrawAPage`:**
  - page 3's top fifth: its aspect, sharp (1000 pixels wide or more), its text dark;
  - its blank middle with no dark pixel;
  - a whole page;
  - the tab not moved, its page and scroll as they were;
  - a page out of range refused;
  - `read_pdf` with `image`: 2 pictures and the text, and a region's picture.
- **`claudesToolsUseItsBar`:** `get_ui` area `document` lists the page field; `set_ui` sets
  it to 3 and page 3 is shown; `get_ui` without an area lists `document`.

Under the sanitizers, `test_pdf_viewer` runs in 20 s.

## Breaks

Each change was broken in turn and the whole `test_pdf_viewer` run, since its functions
share the tab: 17 breaks.

| Of | Breaks |
|---|---|
| find | the first page alone searched; the first hit not made the current one |
| page | an outline's destination by its whole title alone; a page's label not read |
| region | not zoomed to fit; not scrolled to; fractions taken as points |
| zoom | 150 taken as 150 times; a hit not shown again after it |
| screenshot | its region not cut out; the tab moved to the page; a page drawn on black |
| read_pdf | its pictures left out |
| open_document | its page and find not used |
| get_ui, set_ui | no area document; a field not entered |
| show_pdf | the tab not brought to the front |

Two first passed, and their tests were strengthened:

- **A page's label not read.** The test's label, "i", also found "1 Introduction" by a word
  of it. The hand-written PDF's labels are now S-1 and S-2, which no title has.
- **The tab not brought to the front.** The PDF was always in front already. Now another
  tab is in front when `show_pdf` names it.

All 17 are caught.

The full ctest (116 tests) and the MCP scenario script (97 checks) pass.
