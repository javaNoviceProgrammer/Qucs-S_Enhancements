# The hunt of 23 October: its findings fixed

The user, after the hunt: "Let's resolve A1, A2, A3", and then "let's dig into and resolve all of
remaining items". The findings are those of
[2026-10-23-presentations-and-guards.md](../bug_hunts/2026-10-23-presentations-and-guards.md). A1, A2
and A3 - by which a small file can run a command unasked, crash the program or hang it - are the first
three below; A4 to A7, B1, B2, B4, B5 and the notes follow. B3 and N5 are not fixed, and say why.
Throughout, what a fix does not do is said plainly.

| Finding | Now |
|---|---|
| **A1.** A newline in nearly any property wrote lines of its own into the ngspice netlist - a resistor's hidden `Tc1`, a diode's `Area`, a `Vfile`'s `File` - so a `.control` block with `shell` in it; a SPICE model part's `D_Line 2` to `5` did it with no newline at all. Neither `check_schematic` nor the guard of `d14fcdc` named any of it | A value is one token of its line. While a part's netlist is written, the newlines and carriage returns in its values are spaces (`Component::getSpiceNetlist`), except in the parts whose text is SPICE source by design and in a model's continuation lines (`Component::propertyHoldsLines`). The values themselves are untouched: the document keeps what was typed. And the commands check now reads every value of every part, joined, for the commands a `.control` block in them would run - whichever path writes the line - so the guard asks about it first |
| **A2.** A 157 kB deck whose XML nested 50,000 deep (or 20,000 groups) crashed Qucs-S: the reader walks the tree by recursion in four places | A part nesting deeper than 256 is not read, and the deck is refused with that part named ("ppt/slides/slide1.xml nests its elements more than 256 deep (PowerPoint's own nest a few dozen)"). One limit, in `parseXml`, makes every later walk safe |
| **A3.** A 1.8 kB SVG whose `<use>` references expand to 10⁹ nodes hung Qucs-S inside QtSvg, which expands each reference as it reads: as a deck's picture, as a note's image, and opened by itself | `qucs_s::svg::nodes()` (new `svgguard.cpp`) counts what an SVG would draw before QtSvg reads it, and a picture asking for more than 2,000,000 nodes is not drawn. It is counted at every place an SVG reaches QtSvg: a deck's pictures, the picture viewer and its thumbnails, a note's images, a schematic's embedded pictures, and the Claude panel's attachments and transcript |
| **A4.** A chart cache saying `ptCount="2000000000"` filled a list of two thousand million values: killed at 6 GB | A series is read as at most `kMaxPoints` (1,000,000) points, which is what a point's own index was already held to. The deck that was killed reads in 1.0 s and 61 MB |
| **A5.** The runs of one paragraph were laid out again at every run: 5,000 took 36 s to draw a slide, 10,000 took 145 s, 100,000 never finished | The text is put in between `beginEditBlock()` and `endEditBlock()`, and the document keeps no undo, so it is laid out once: 5,000 runs now take 0.14 s (260 times less), 10,000 take 0.48 s, 20,000 take 1.8 s where they did not finish |
| **A6.** The program was killed as it quit while a tab's own Export the Slide dialog was open: freeing the tab freed the dialog, which is a local object of the call that opened it | A document says, while one of its own dialogs runs, that it is in one (`QucsDoc::inADialog`, held by `InADialog` around the call). `closeAllFiles` frees such a document once its own call has returned (`deleteLater`), and the others as before |
| **A7.** In the text the commands check does read, a shell reached otherwise than by a line beginning `shell`, `system` or `!` was missed: an alias, a backquote, a variable in the command's place | Each of those three is now named, read from ngspice 46's own sources (a backquote goes to `popen`; an alias and a variable are expanded before the command is looked up). The `;` and the bare carriage return of the hunt's table are **not** holes - ngspice reads `;` outside brackets as part of a word and drops a lone `\r` - so the hunt's claim about them was wrong and is corrected there |
| **B1.** A shadow of more than 16 million device pixels was not drawn at all: a slide-sized shape lost its shadow at an export of 8 pixels a point, and at 800 % in the tab | The mask is built for what can be seen (the device, or what the painter clips to, with a blur's margin around it), and a mask still larger than that is built smaller and drawn scaled up - which a blurred shadow does not show. No shadow is skipped: the probe deck now has it at scales 1, 4 and 8 |
| **B2.** `show_pptx` with `slide` and `zoom` together ended on the slide before | The root of it: `SlideView::setZoom` kept the slide's place so exactly that a slide at the view's very top opened a hair above it, on the end of the slide before - which `updateCurrent()` then called the slide shown. The view no longer opens above the slide it keeps |
| **B4.** "all" slides, or a longer range, was cut at 500 and the answer said nothing | `export_image` and `read_pptx` say what they left out ("2500 slides more were asked for: 500 at a time at most - ask for the rest by their numbers") |
| **B5.** Twenty `int(node->num(...))` casts were undefined for a number past `int`'s range | `Node::whole(name, fallback, lo, hi)` holds the number within its bounds before converting it, and every one of the twenty uses it |
| **N1.** A step at an end moved nothing and said nothing | `show_pptx` says "Already the last slide (7)." or "Already the first slide." |
| **N2.** `export_image`'s schema said 0.1 to 20, a presentation refuses more than 12 | The schema says both: "A presentation's slides: pixels a point, 0.1 to 12" |
| **N3.** A name with a right-to-left override was taken | `badFileName` refuses a name holding a character that is not drawn but changes how the name reads (the bidi overrides and isolates, the zero-width spaces, the byte-order mark) |
| **N4.** After a reload the find bar's answer called a hit "shown" while the tab was elsewhere | It is "shown" only where the tab is on that slide; otherwise it is "next" |
| **N6.** `open_document` with an out-of-range `page` answered as an error, though the deck was open | The deck is reported open, with what was wrong with `page` after it |
| **N7.** `set_ui` answered "Set" for a value a field had refused or held | Where a field of numbers (or an editable list) took something else, the answer says so: "Zoom (it took 100, not 99999)". `fillControls` is shared, so `set_dialog` says it too. The presentation tab's own Zoom and Slide are a list and a line to type in, whose value takes effect on Return, so those two still answer "Set" - `show_pptx` is the checked way to them |

## A1. A value writes no line of the netlist

Two halves, because the netlist has more than one path into it.

**The netlist.** `Component::getSpiceNetlist()` is the one place every part's netlist text comes
through. It now holds the part's values on one line while `spice_netlist()` or `cdl_netlist()` runs
(`ValuesOnOneLine`, which puts them back), so none of the ~200 implementations can write a line it did
not mean. `Component::propertyHoldsLines(name)` says which values are lines by design:

- the parts whose text is SPICE source: `INCLSCR`, `.CUSTOMSIM`, `.XYCESCR`, `CMD`, `NutmegEq`,
  `SPICEINIT`, `SpiceModel`;
- a model's continuation lines, named `*_Line <n>` (`D_Line 2`, `Model_Line 3`), which the netlist
  writes one to a line anyway.

**The check.** `erc::commandsOf()` now reads every value of every part - all of them joined, since a
block can span several properties (`D_Line 2` holding `.control` and `D_Line 3` holding `shell`) - and
names the commands a `.control` block in them would run. The parts whose own text it already reads are
left to those branches. So whichever path writes the line, `check_schematic` names it and the guard of
`d14fcdc` asks before the schematic is simulated.

**Checked over every part.** `propscan_all.py` (in the hunt's folder) sets each property of each of the
232 component types in turn to a value holding a `.control` block, and looks for a netlist line of its
own and for what the check says:

| | before | after the netlist half | now |
|---|---|---|---|
| values writing a line of their own that the check does not name | 14 + the rest | 14 | **0** |
| values writing a line of their own, named by the check | 0 | 105 | 119 |

The 14 that the netlist half did not reach are written by paths of their own, which is how they were
found: a global parameter, a `.PARAM`, a `.FUNC`, an `.IC`, a `.NODESET`, an `.OPTIONS` and an
`.INCLUDE`'s or `.LIB`'s file name go through `getExpression()` and the kernel's own writing, not
through `spice_netlist()`. Reading every value covers them.

**Nothing else changed.** Every example schematic was netlisted for ngspice and for Xyce before and
after: 504 netlists of 253 schematics, byte for byte the same.

*Not done.* A value can still hold a `.control` block; it is kept on one line and asked about, not
refused. Refusing it belongs where the value is set (the dialog, the loader, the tools), and would need
to know which properties may hold lines - the same table, enforced in more places. The paths that write
their own lines (`getExpression()` and the kernel's) still take a value as written; the check is what
covers them.

## A2. A deck nesting too deep

`parseXml()` stops at `kMaxXmlDepth` (256) and says which part it was; `Presentation::read()` refuses
the deck. The four walkers that recurse - `mc:AlternateContent`, groups in `buildTree`, `notesOf`, and
`~Node` - are all safe once no tree is deeper than that.

PowerPoint's own decks nest a few dozen deep: the 270 decks on this machine (4,587 slides) are read and
drawn as before, under AddressSanitizer and UndefinedBehaviorSanitizer with no report, and the limit
refuses none of them. 256 leaves room for several times the deepest.

*Not done.* The limit is on depth, not on the number of elements (B3 of the hunt: a part's XML is held
at about 37 times its size). A deck is still refused whole rather than drawn as far as it goes.

## A3. An SVG that asks for too much

`qucs_s::svg::nodes(bytes)` counts what the SVG would draw once every `<use>` reference is expanded:

- each element belongs to the nearest element with an `id` of its own (or to the document);
- a `defs`, `symbol`, `marker`, `clipPath`, `mask`, `pattern` or gradient is counted only where
  something refers to it;
- a reference that leads round to itself, and one nothing defines, count as nothing - QtSvg stops at
  those too;
- the count saturates at `kMaxNodes` (2,000,000), so a reference graph of 10⁹ costs nothing to weigh;
- an `.svgz` is inflated first (its gzip header taken off, then `zip::inflate` with a limit of 64 MB);
- a file with no `<use` in it at all returns 0 straight away: its work is its size, which QtSvg handles
  linearly. That makes the count free for every ordinary picture (a `memmem` over the bytes).

**Where the limit sits.** Decks of one `<use>` fanning out four ways, drawn here:

| levels | nodes | read | drawn |
|---|---|---|---|
| 9 | 0.7 M | 0.05 s | 0.05 s |
| 10 | 2.8 M | 0.1 s | 0.4 s |
| 11 | 11 M | 0.5 s | 1.7 s |
| 12 | 45 M | 2.2 s | 6.6 s |
| 15 | 2.9 G | never finished | |

2,000,000 is the knee: level 9 is drawn, level 10 and up are not. Every repaint pays the drawing cost,
so a picture of a few million nodes is not one to keep.

**Every SVG on this machine** - 225 files in the repository, the workspace and the Desktop - was
counted in 11 ms. None is refused but the hunt's own, and four of them hold a `<use>` at all.

**Counted at each place one is read**, since each reads by content, not by name:

| | |
|---|---|
| a deck's pictures | `pptx.cpp` `Media::decode` - before the kind is dispatched, since `QImage::fromData` reads an SVG named `.png` all the same; a box in its place, and the slide says "an SVG picture is not drawn: it asks for more than 2,000,000 nodes through its `<use>` references" |
| the picture viewer | `ImageDoc::read` (the tab says why) and `qucs_s::image::thumbnail` (the file browser's) |
| a note's images | `markdowndoc.cpp`'s `loadResource` |
| a schematic's embedded pictures | `EmbeddedImage::decode` |
| the Claude panel | `imageForClaude` (not sent, with the reason), `isPicture` (not taken as one), and the transcript's own pictures ("[name: not drawn]") |

### Found while fixing

- **R1.** A note's `![x](bomb.svg)` - a relative name - never reached the viewer's own
  `loadResource`: that override only looked at local files, and Qt's base class fetches a relative name
  itself. The guard there did nothing until it was moved to what the base class returned.
- **R2.** Then it still hung: `QTextDocument::loadResource` reads the file **itself** when the
  resource comes back empty, and that read is the one to avoid. A 1×1 transparent picture in its place
  stops it.
- **R3.** A deck's picture part named `image1.png` holding SVG bytes hung too: the reader dispatches
  on the suffix, but `QImage::fromData` decides by content. The count had to come before the dispatch.
  The same held for the picture viewer, which reads with `setDecideFormatFromContent(true)`.
- **R4.** The count began by looking for `<use` in the bytes, to cost nothing for the ordinary
  picture. A prefixed name - `<svg:use>`, which is what Inkscape writes when it keeps the SVG
  namespace prefixed - was not found, and QtSvg honours it: the same deck prefixed took the same 2.4 s
  to read (level 12), so the guard had a way round it. It now looks for `:use` as well, and reads
  namespaces as QtSvg does, so `xlink:href` and SVG 2's plain `href` are both followed. Measured on
  the bomb: `xlink:href` 144 s in QtSvg before, each form now counted and refused.

*Not done.* The count is of nodes, not of time: an SVG within the limit that is slow for another reason
(a huge filter chain, say) is still drawn. QtSvg reads on the GUI thread, as before.

## B3 and N5: not fixed

**B3, a part's XML held at about 37 times its size.** Measured again, and not fixed. The names were
the first suspect - a chart part holds `c:pt` a million times - so each name was held once, in a pool:
1,511 MB became 1,459 MB, 3 %. The bulk is the tree itself (a `Node` for each element, with its
attributes in a list of its own), so the fix is structural - chart caches read as a stream, or the tree
in an arena - not a change of a few lines, and nothing crashes meanwhile: A4 now bounds what a cache
asks for, and the archive bounds a part at 128 MB. The pool was taken back out, to leave no code that
earns nothing.

**N5, a deck with one damaged part refused whole.** Left as it is, deliberately. `zip::read` refuses an
archive in which any entry cannot be inflated, which is what keeps a damaged deck from being drawn half
way; PowerPoint instead offers to repair it. Making Qucs-S skip a part and go on means deciding which
parts a deck can do without, part by part, and that is a change to the archive reader that wants its own
round.

## Tests

| test | what it holds to |
|---|---|
| `test_erc` `aValueWritesNoLineOfTheNetlist` | a resistor's `Tc1` with newlines, and with carriage returns, stays on R1's line; an equation's and a transient's too; an Include Script's text keeps its lines; a `D_Line` block is named by the check, and so is a global parameter's; a plain resistor names nothing |
| `test_pptx_doc` `deepNestingIsRefused` | 40 groups deep read and drawn; 5,000 refused with the part named; 400 elements of no known kind refused, the limit said |
| `test_pptx_doc` `svgAskingTooMuchIsNotDrawn` | the count itself (the bomb, a square named twice, a cycle, an unknown name, a PNG); in a deck, named `.svg` and named `.png`, drawn within 10 s, the note said, and the SVG beside it still green |
| `test_image_doc` `anSvgAskingTooMuchIsRefused` | not opened, said why, no thumbnail, within 10 s, named `.svg` and `.png`; the ordinary SVG still opens |
| `test_markdown_doc` `anSvgAskingTooMuchIsLeftOut` | the note opens within 20 s, the other two pictures at their own size, the one asking too much not at its |
| `test_claude_code` `imagesDroppedAreShownToClaude` | an attachment asking too much is not sent, with the reason, within 10 s |
| `test_qucs_control` `guardsAskAboutCommandsAndFilesOutside` | the guard asks before simulating a schematic whose command is in an ordinary value, and one whose command is in a model's continuation lines |
| `test_erc` `commandsByAnotherNameAreSaid` | an alias, a backquote and a variable in the command's place are each named; a block that only plots and prints is not; `;` and a bare carriage return divide no line |
| `test_pptx_doc` `absurdNumbersAreHeld` | a cache of two thousand million points, a point's index, a paragraph's level, a run's baseline, a bar's gap and overlap: read, drawn, within 30 s (and no report under UndefinedBehaviorSanitizer) |
| `test_pptx_doc` `manyRunsAreLaidOutOnce` | 5,000 runs in one paragraph drawn within 10 s (36,000 ms before, some 140 ms now), and all of the text there |
| `test_pptx_doc` `aTabIsNotFreedUnderItsOwnDialog` | the window closed from inside the tab's own save panel: the tab goes, nothing is freed under the dialog, and the call returns |
| `test_file_browser` `renamingChoosesTheNameNotTheSuffix` | in place: "amp" of "amp.sch", "data.tar" of "data.tar.gz", the whole of ".spiceinit", of "plain" and of a folder's "folder.old" |
| `test_file_browser` `refreshReadsTheFoldersAgain` | a file and a folder written behind the watcher's back appear on Refresh, the folder shown and the choice kept |
| `test_file_browser` `aLinkedFolderIsShownAndUnlinked` | a linked folder is shown, listed among the links here, entered, and unlinked - leaving what it led to |
| `test_file_browser` `aLinkedFoldersFilesBelongToTheProject` | `misc::projectFiles` holds `shared/brought.sch` and `shared/sub/deeper.txt`; a link that leads back to the project ends the walk |
| `test_file_browser` `entriesAreMadeAndTheirMenusOffered` | Refresh, Import Folder and Link Folder in the menus; Unlink Folder only where a link is there |

The suites that use what was touched pass: `test_erc` (25), `test_pptx_doc`, `test_image_doc`,
`test_markdown_doc`, `test_claude_code`, `test_qucs_control`, `test_mcp_server`, `test_netlist_audit`,
`test_netlist_fixes`, `test_symbol_image`, `test_loader`, `test_spice_options`, `test_simulator_modes`,
`test_subcircuit_dialog`, `test_file_browser`.
