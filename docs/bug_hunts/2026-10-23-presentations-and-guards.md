# Bug hunt: the presentation viewer and its tools, and the auto mode guards

*23 October 2026 - Qucs-S 27.0.3, `f9164f1` (the Release build), and the ASan+UBSan `test_pptx_doc` of
`1e39afe` (the presentation code is the same in both). In the foreground, hunting from 06:11 to 07:00,
the write-up after. The probes are scripts in [2026-10-23-presentations-and-guards/](2026-10-23-presentations-and-guards/).
They drive the real MCP server (`qucs-s --mcp-server`, offscreen, with its own HOME, settings and
cache) and `test_pptx_doc`'s `renderDecksGiven`. The findings are written up as they came; none is
fixed here.*

The user's request: "continue hunting for bugs in our enhanced qucs ... run fuzz tests and document your
findings in "bug_hunts" folder ... After one full hour, please stop and provide a summary report."

The hunt looked first at what changed last and had not been hunted:
- `d37ee17`: the presentation viewer;
- `519f625`: Claude's control of it;
- `1e39afe`: the review round (shadows, fonts, chart axes, clouds, the view kept on reload);
- `d14fcdc`: the two auto mode guards (a schematic's commands, files outside the workspace).

What ran:
- **A new deck fuzzer** ([fuzz_pptx.py](2026-10-23-presentations-and-guards/fuzz_pptx.py)): 1,500 mutants
  of five decks, in three runs (300, 600, 600), drawn under AddressSanitizer and UndefinedBehaviorSanitizer
  with a memory watchdog. It mutates XML (numbers, deletions, duplicates, preset names, text, nesting,
  relationships, slide sizes, chart caches, tables, custom geometry, shadows, bullets, entities, geometry)
  and the archive itself (truncation, damaged tails, deflate bombs).
- **A new fuzzer of the presentation tools' arguments** ([fuzz_mcp.py](2026-10-23-presentations-and-guards/fuzz_mcp.py)):
  2,000 calls of `read_pptx`, `show_pptx`, `export_image`, `screenshot`, `context_menu`, `open_document`,
  `list_documents` and `get_state`, through the real server. It used four seeds: wrong types, then mostly
  valid arguments with one bad value.
- **Decks made by hand**: deep nesting, relationship cycles, hostile pictures, size and speed.
- **The guards read and probed** through the server.
- **`fuzz-sch.py`**: 400 mutants, seed 2310.

No probe ran a command. The schematics of A1 were netlisted and checked, never simulated.

## Summary

| | Finding | Since |
|---|---|---|
| **A1** | A newline in an ordinary property puts lines of its own into the ngspice netlist. A resistor's `Tc1` of `0\n.control\nshell true\n.endc` writes a `.control` block that runs a shell command. Every part type tried breaks lines so, from some or all of its properties; a SPICE part's `D_Line 2` to `5` write such a block with no newline at all. `check_schematic` names no command (with its check of commands on), and the commands guard asks only about what `erc::commandsRun()` finds, which never looks at ordinary properties. A `.sch` file keeps the newline (`\n`) and loads it back, so a schematic from someone else can carry it | before (the netlister); the guard since `d14fcdc` |
| **A2** | A deck whose XML nests deeply crashes Qucs-S (stack overflow). A 157 kB deck with 50,000 nested elements, or 20,000 nested groups, crashes the Release build; 3,000 groups crash the ASan build. Several walkers recurse with no limit | `d37ee17` |
| **A3** | An SVG whose `<use>` elements expand exponentially (1.8 kB) hangs Qucs-S inside Qt's SVG reader. It hangs as a deck's picture, as a Markdown note's image, and opened by itself | `d37ee17` for decks; before for the others |
| **A4** | A chart's `ptCount` is not bounded (a point's `idx` is, at 1,000,000). `ptCount val="2000000000"` fills a list of 2×10⁹ values: killed at 6 GB | `d37ee17` |
| **A5** | The runs of one paragraph are laid out in quadratic time: 5,000 runs take 36 s to draw a slide, 10,000 take 145 s, and 100,000 (166 kB) were killed after 120 s. Each run's `insertText` lays the document out again | `d37ee17` |
| **A6** | The server aborts as it quits while the tab's Export the Slide dialog is open ("pointer being freed was not allocated", in `PptxDoc::~PptxDoc`). Seen offscreen, when the client's input closes | `d37ee17` |
| **A7** | In SPICE text the check does read (an Include Script), it misses a shell reached otherwise than by a line starting `shell`, `system` or `!`. An alias (`alias zz shell`, then `zz true`), a backquote (``echo `true` ``), a command after `;`, or a variable naming the command (`set c = shell`, then `$c true`) is not flagged, so not asked about. The parked wishlist of 22 October names the alias and the backquote | `d14fcdc` (the guard); the check before |
| **B1** | Shadows disappear at a high zoom or export scale. A shadow larger than 16 million device pixels is skipped: a 720×360 pt shape's shadow is drawn at export scale 4, missing at 8 (and at 800 %, or 400 % on a Retina screen) | `1e39afe` |
| **B2** | `show_pptx` with `slide` and `zoom` together, from "fit width", shows the slide before: `{"slide": 5, "zoom": 100}` ends on 4, `{"slide": 7, "zoom": 300}` on 6. Each alone is right | `519f625` |
| **B3** | A deck's XML is held at about 37 times its size: a chart of a million points (38.8 MB of XML) peaks at 1.5 GB. Within the archive's limits (128 MB a part, 256 MB in all), that is some 9 GB | `d37ee17` |
| **B4** | "all" slides (or a longer range) is cut at 500, without a word: a 3,000-slide deck gave a 500-page PDF and 500 pictures, and the answer lists the 500 as if they were all | `519f625` |
| **B5** | Twenty `int(node->num(...))` casts of the reader are undefined for a number past `int` (UBSan: "1e+11 is outside the range of representable values of type 'int'", at `pptx.cpp:1969`, `2091`, `2095`); the bounds come after the cast | `d37ee17` |
| **N1** | `show_pptx` `slide` "next" on the last slide, "previous" on the first, "" or null: nothing changes and nothing says so | `519f625` |
| **N2** | `export_image`'s `scale` says 0.1 to 20 in its schema; a presentation refuses more than 12 | `519f625` |
| **N3** | `new_project` takes a name with a right-to-left override (U+202E): the folder `‮evil_prj` shows reversed. Control characters below U+0020 are refused, but Unicode's format characters are not | before |
| **N4** | After a reload, the find bar's answer says `"shown": "slide 7, the 1 of 1"` while the tab is on slide 2 | `1e39afe` |
| **N5** | One part that cannot be inflated, or is larger than the archive says, refuses the whole deck ("not read"). PowerPoint offers to repair such a deck and shows the rest | `d37ee17` |
| **N6** | `open_document` with an out-of-range `page` answers as an error, though it opened the deck | `519f625` |
| **N7** | `set_ui` answers "Set" for a zoom or slide the tab's field refuses or clamps ("abc", 999) | before |

Fix A1 first, then A7. Each undoes what `d14fcdc`'s guard promises, and `check_schematic`'s
commands warning too, in two cases:
- a schematic Claude edits in auto mode (`edit_component` and `simulate` are both allowed unasked);
- a schematic received from someone else.

A2 to A5 let a small file freeze or crash the program. A2 and A3 can do so through `read_pptx` and
`open_document` alone.

*Later the same day:* every finding but B3 and N5 is fixed (the user: "Let's resolve A1, A2, A3", then
"let's dig into and resolve all of remaining items"). See
[2026-10-23-hunt-fixes-a1-a2-a3.md](../feature_gaps/2026-10-23-hunt-fixes-a1-a2-a3.md). A1's fix also
covers the netlist's other paths, found while fixing it: a global parameter, an `.OPTIONS`, an `.IC`, a
`.NODESET`, a `.FUNC` and an `.INCLUDE`'s file name write their own lines, so the commands check now
reads every value of every part. **B3** (a part's XML held at 37 times its size) and **N5** (a deck with
one damaged part refused whole) are not fixed, and the write-up says why. A7's table above claimed two
holes that are not holes, corrected in place below.

## A. Wrong or unsafe without a word

**A1. A newline in a property is a line of the netlist.**
`edit_component` set R1's `Tc1` to `"0\n.control\nshell true\n.endc\n"`. `get_netlist` then gave, line
by line ([calls/c8.json](2026-10-23-presentations-and-guards/calls/c8.json)):

```
R1 0 _net0  1K tc1=0
.control
shell true
.endc
 tc2=0
```

- `check_schematic`, with the setting "Warn of commands a simulation runs besides the simulator"
  turned on (it is off by default, and without it the check names no command at all): nothing about a
  command ([calls/c23.json](2026-10-23-presentations-and-guards/calls/c23.json), its last call). The
  same check names the same `shell true` in an Include Script.
- `erc::commandsOf()` (`erc.cpp:1799`) looks only at the parts that bring SPICE text in on purpose:
  libraries, includes, subcircuits, the Include Script, the System Command, `.CUSTOMSIM`, `NutmegEq`
  and the SPICE init part.
- The guard (`QucsControl::commandsNamed()`) and `runRefused()` ask about exactly what
  `erc::commandsRun()` finds.

So in auto mode, `edit_component` (allowed unasked) can add a shell command that `simulate` (allowed
unasked) runs, with no question. Probe c9 saved the schematic and opened it again. The file holds
`"0\n.control\nshell true\n.endc\n"` as `\n` escapes on R1's line
([newline-in-tc1.sch](2026-10-23-presentations-and-guards/newline-in-tc1.sch)), and the reopened
schematic netlists the same block. A schematic from a colleague, a forum or a repository can carry it,
with nothing shown on the canvas: `Tc1` is a hidden property.

The same happens elsewhere, less directly ([calls/c10.json](2026-10-23-presentations-and-guards/calls/c10.json)):
- **R's own value.** The netlister uppercases it and takes the spaces out (`1K`, `.CONTROL`,
  `SHELLTRUE`, `.ENDC`), but the lines still break.
- **A transient's `MaxStep`.** Its line is written inside the netlist's own `.control` block, so each
  line after the newline is a control command (spaces taken out).
- **Refused:** names (`rename`) and net labels (`set_label`).

**Every part tried.** [propscan.py](2026-10-23-presentations-and-guards/propscan.py) set each property
of ten part types in turn to `1\n.control\nshell echo marker\n.endc\n`, then read `get_netlist`. In
every type tried, some or all properties broke lines into the netlist:

| part | properties that came out verbatim, spaces kept |
|---|---|
| Diode | `Area` |
| D Diode (`DIODE_SPICE`) | `D`, `D_Line 2` to `D_Line 5`, `Letter` |
| File Based Voltage Source (`Vfile`) | `File`, `G`, `T` |
| VCVS | `G` |
| R | `Tc1` |

The others broke lines too, but uppercased and with spaces taken out: every model parameter of the
Diode and both MOSFETs, `Vdc`'s `U`, `Vac`'s seven, `C`'s `C` and `V`, `L`'s `L` and `I`.

**With no newline at all.** The SPICE parts' `D_Line 2` to `D_Line 5` are the model's extra lines by
design, and `commandsOf()` does not read them. Probe c31 set the D Diode's `D_Line 2`, `3` and `4` to
`.control`, `shell echo marker` and `.endc`. With the check of commands on, `get_netlist` gave
`D1 _net0 _net1` followed by those three lines, and `check_schematic` named no command.

Nothing was simulated, so it is not shown here that ngspice runs the block. The netlist is what a
simulation writes ("as a simulation would write it now"), and ngspice runs a `.control` block wherever it
stands.

*Proposed fix:*
- The netlister writes no line break from a property value: a newline in a value becomes a space, or is
  refused when the value is set. The few properties that hold SPICE text on purpose (Include Script, the
  equation-like parts) keep theirs.
- `commandsOf()` reads the SPICE parts' `*_Line` properties as it reads an Include Script.
- `edit_component`, `set_schematic` and the loader refuse or flag a newline in any other property.
- Until then, `commandsOf()` could look for `.control`, `shell` and the other run patterns in every
  property's value.

**A2. Deep nesting crashes the reader.** Several functions walk the XML tree by recursion with no limit:
- `parseXml`'s `mc:AlternateContent` pass (`pptx.cpp:173`);
- `buildTree` for groups (`pptx.cpp:2800`);
- `notesOf` (`pptx.cpp:3817`);
- and, by its shape, `~Node`.

Probes (Release, [mkprobe.py](2026-10-23-presentations-and-guards/mkprobe.py)):
- **Nested elements** (`<a:x>` inside `spTree`): 10,000 deep is fine; 50,000 deep crashes (SIGSEGV in
  `parseXml`'s lambda). The deck is 157 kB.
- **Nested groups:** 3,000 are fine in Release; 20,000 crash (SIGSEGV, stack exhausted in `buildTree`).
  Under ASan, 3,000 already overflow the stack.

Opening such a deck in a tab, `read_pptx` and `export_image` all crash the program. 11 of the fuzzer's
1,500 mutants found it (`nest`), through slides, layouts and notes. *Proposed fix:* a depth limit in
`parseXml` (a deck nests a few dozen deep; refuse past, say, 256) makes every later walk safe. Or make
the walkers iterative.

**A3. An exponential SVG hangs the program.** [use-expand.svg](2026-10-23-presentations-and-guards/use-expand.svg)
is 1.8 kB. Each of its 15 levels uses the level below four times, so it draws 4¹⁵ (about 10⁹)
rectangles. The sample of the hang:

`Presentation::read` → `buildPicture` → `Media::decode` → `QSvgIOHandler::read` → `QSvgRenderer::load` →
`QSvgDocument::load`

It hangs at reading, on the GUI thread. Each of these hung past its time limit:
- a deck with it as a picture: past 120 s;
- `open_document` of a Markdown note showing it as an image: past 40 s;
- `open_document` of the `.svg` itself: past 30 s.

A plain SVG in the same note opened at once. Qt's own guards (a use cycle, billion laughs, a huge
canvas) held: those decks were drawn at once. *Proposed fix:* before handing an SVG to QtSvg, count its
`<use>` references and their fan-out (or refuse an SVG whose `use` graph expands past some 100,000
nodes). Or load it on a worker thread with a time limit, and show a placeholder.

**A4. A chart's point count unbounded.** In `numbers()` and `strings()` (`pptx.cpp:2091`, `2119`,
`2137`), `size = max(ptCount, most + 1)`, and `ptCount` is taken as written. Mutant m00017 (run 1) had
`ptCount val="2000000000"`. The ASan build was killed by the watchdog at 5.9 GB after 98 s. With
`ptCount="3e10"` (probe `intcast`), the cast saturates and the same happens: killed at 7 GB. *Proposed
fix:* bound `ptCount` as `idx` is (1,000,000), or by the points present.

**A5. Many runs in a paragraph: quadratic.** `perf/runs*k.pptx` alternates bold and italic runs of one
letter in one paragraph:

| runs | slide drawn in |
|---|---|
| 5,000 | 36.5 s |
| 10,000 | 145 s |
| 100,000 | killed at 120 s |

The sample shows `TextFrame::document()` → `QTextCursor::insertText` → `QTextDocumentPrivate::finishEdit`
→ `QTextDocumentLayout::documentChanged` → `doLayout`: the document is laid out again after every run.
Code listings with syntax colouring, or text pasted from the web, carry thousands of runs. At the same
rate, a thousand would cost some 1.5 s a drawing (estimated, not measured). *Proposed fix:* build the document before it has a layout, or
inside `beginEditBlock()`/`endEditBlock()`, and lay it out once.

**A6. An abort at quitting with the Export dialog open.**
1. `context_menu` `{"on": {"slide": 2}, "choose": "Export the Slide as a Picture"}` opened the dialog,
   and the call answered with the dialog's controls, as designed.
2. The client then closed its input.
3. The server quit with SIGABRT.

The backtrace:

`QApplication::closeAllWindows` → `QucsApp::closeEvent` → `closeAllFiles` → `PptxDoc::~PptxDoc` →
`QObjectPrivate::deleteChildren` → `___BUG_IN_CLIENT_OF_LIBMALLOC_POINTER_BEING_FREED_WAS_NOT_ALLOCATED`

`pptxdoc.cpp:1350` calls `QFileDialog::getSaveFileName(this, ...)`. Offscreen, that is Qt's own dialog,
an object on the stack whose parent is the tab, so deleting the tab frees it. It was seen offscreen; on
the display the native panel may behave otherwise (not tried). The same pattern, a static file dialog
parented to a document, is used elsewhere in Qucs-S. *Proposed fix:* do not delete documents while a
modal dialog runs on one: close the dialog first (`QApplication::activeModalWidget()->reject()`), or
defer the close until it returns.

**A7. The commands check reads lines, not what ngspice runs.** `controlCommands()` (`erc.cpp:1614`)
flags a line of a `.control` block that starts with `shell`, `system` or `!` (`programRun()`), or with
`source`, `codemodel`, `osdi` or their `pre_` forms (`codeLoaded()`). Probe c23 set an Include
Script's text to each form below, with the check of commands on, and read `check_schematic`.

| the script's `.control` block | flagged |
|---|---|
| `shell true` | yes |
| `shell<tab>true` | yes |
| `SHELL true` (and `.CONTROL`) | yes |
| `shell true` inside `if 1` ... `end` | yes |
| `shell true` with no `.endc` | yes |
| `alias zz shell`, then `zz true` | no |
| ``echo `true` `` (a backquote) | no |
| `echo a ; shell true` | no (and it runs nothing: see below) |
| `set c = shell`, then `$c true` | no |
| the lines split by carriage returns alone | no (and they are no lines: see below) |

ngspice's control language has aliases, backquote substitution and variables.

*Corrected while fixing this (23 October).* ngspice 46's own sources settle what each of these does,
and two of the five above are **not** holes - this table claimed more than it should have:
- a backquote **does** run a command: `cp_bquote` passes what stands between them to `popen`
  (`src/frontend/parser/backq.c`);
- an **alias** is expanded before the command is looked up (`parser/cshpar.c`, `cp_doalias`), and a
  **variable** where the command's name belongs is substituted first (`control.c`,
  `cp_variablesubst`) - a line beginning `$` is no comment, since only `#` and `*` begin one
  (`parser/lexical.c`);
- a **`;`** outside brackets is part of a word, not another command (`parser/lexical.c`, its `case
  ';'`), so `echo a ; shell true` runs nothing;
- a bare **carriage return** is dropped as it is read (`frontend/inpcom.c`, `if (c == '\r') continue;`),
  so it divides no line.

The three that are holes are now named by the check. The guard asks about what this check finds, so a form it misses runs unasked
in auto mode. The parked wishlist of 22 October names the alias and the backquote.

*Proposed fix:* a pattern for each line can be got round this way. Taking any `.control` block in text
a schematic brings in as "runs commands" would leave nothing to get round: a block that only plots or
prints would be asked about once, as the guard asks.

## B. Less than right

**B1. Shadows lost at high resolution.** `drawShadow()` (`pptx.cpp:2980`) returns without drawing when
the silhouette's device rectangle is above 16,000,000 pixels. The rectangle is the whole shape, not the
part being painted.

The probe ([calls/c5.json](2026-10-23-presentations-and-guards/calls/c5.json)) is a 720×360 pt shape
with a red shadow 30 pt down and right. At the point (766, 406) pt, inside the shadow and outside the
shape:

| export scale | pixel |
|---|---|
| 1 | red |
| 4 | red |
| 8 | white |

The tab paints at its zoom times the screen's ratio, so the shadow vanishes there too: at 800 %, or at
400 % on a Retina screen. *Proposed fix:* clip the mask to the painter's clip, or draw it at a lower
resolution and scale it up, instead of skipping it.

**B2. `slide` with `zoom` lands one slide early.** Starting from a freshly opened deck (fit width), each
call ran alone:
- `{"slide": 5, "zoom": 100}`: slide shown 4 ("Charts").
- `{"slide": 7, "zoom": 300}`: slide shown 6.
- `{"slide": 5}` and then `{"zoom": 100}`: 5.
- `{"slide": 3, "zoom": 250}` from 5 at 100 %: 3.

The slide is set first, and the zoom then keeps the old view's place, which is in the slide before.
*Proposed fix:* apply `zoom` (and `region`) before `slide`, or go to the slide again after the zoom.

**B3. The tree's memory.** `perf/chart1_million.pptx` has a 3.7 MB archive, with the chart's part
inflated to 38.8 MB. It peaked at 1,511 MB (Release, `renderDecksGiven`), about 37 times the XML. Each
element is a `Node` with its own `QString` name, `QXmlStreamAttributes` and text. The inflate limits
allow 256 MB of XML, so a deck within them can take some 9 GB. *Proposed fix:* count the elements while
parsing and refuse a part past some millions; or read chart caches as a stream.

**B4. 500 slides at most, unsaid.** `qucscontrol_pptx.cpp:48` and `59` stop a list or a range at 500.
`export_image` of "all" on a 3,000-slide deck wrote `s-0001.png` to `s-0500.png`. The answer had
`"slides"` and `"written"` of 500 entries and no note; the PDF had 500 pages. *Proposed fix:* refuse
with "at most 500 slides a call", or say what was left out.

**B5. Undefined casts.** UBSan reported, on probe `intcast`:
- `pptx.cpp:1969` (`lvl="99999999999"`);
- `pptx.cpp:2091` (`ptCount="3e10"`);
- `pptx.cpp:2095` (`idx="99999999999"`).

There are 20 casts of the form `int(x->num(...))`, each clamped or bounded after the cast. On arm64
the conversion saturates, so the bounds still work there by luck. *Proposed fix:* an `intOf(node,
name, lo, hi, fallback)` that clamps the double before converting.

## Notes

- **N1.** `next` at the end and `previous` at the start answer like a move. Claude cannot tell that
  nothing moved, except by comparing "slide shown". A `"note"` ("already the last slide") would say it.
  `slide: ""` and `null` are taken as no `slide` at all.
- **N2.** The schema is shared with schematics' exports (up to 20). The presentation's limit of 12 is
  said only when refused.
- **N3.** `new_project` refuses `<>:"/\|?*` and control characters. U+202E and the other bidi controls
  and zero-width characters are let through and shown in the project view as written.
- **N4.** After the file was replaced and read again, `"find bar": {"shown": "slide 7, the 1 of 1"}`
  while `"its title"` was slide 2's. The bar counts its hits again without moving (R1 of
  `1e39afe`), but its answer still calls the old hit "shown".
- **N5.** A deck with one damaged picture is refused whole ("ppt/media/image1.png cannot be inflated,
  or is bigger than the archive says"). That is safe, but stricter than PowerPoint.
- **N6.** `open_document` of a deck with `page` 0 or 99 answers as an error ("viewer_test.pptx has
  slides 1 to 7"), though the deck was opened and is in front. A client that retries an error opens it
  again. With `find: ""` it says " is not in its slides' text or notes."
  ([calls/c24.json](2026-10-23-presentations-and-guards/calls/c24.json)).
- **N7.** `set_ui` answers "Set: Zoom." for a zoom of "-5", "99999" or "abc", and "Set: Slide." for "0",
  "999" or "3.7". The tab's fields clamp or ignore these: slide 999 went to 7
  ([calls/c28.json](2026-10-23-presentations-and-guards/calls/c28.json)). `show_pptx` refuses the same
  values with a reason.

## Checked and found right

- **The deck fuzzer.** Run 1 (300 mutants) and run 2 (600) found only A2 and A4. Run 3 (600, with
  nesting at most 40 deep, and geometry, dash, line width and rotation mutations added) found nothing:
  no crash, sanitizer report, hang or memory blow-up.
- **Archives.** Checked under ASan, Release or both:
  - A ZIP64 archive: refused.
  - A picture inflating to 1 GB or 2 GB: refused, "at most 128 MB a file, 256 MB in all".
  - Sizes understated in both headers (1,000 and 0 bytes for 1 GB): refused, with no overrun.
  - Truncated archives and damaged tails: refused.
- **Pictures.**
  - PNGs claiming 100,000² and 30,000² pixels, and an 8,000² full one: drawn, at most 78 MB.
  - SVG billion laughs, a 10⁸-pixel SVG canvas, a 100,000-deep SVG, a `use` cycle: drawn at once.
- **Relationship cycles.**
  - A layout whose master is itself, or a slide.
  - A slide whose layout is itself.
  - A master whose theme is the master.
  - A slide's notes that are the slide.
  - A layout that is a chart, a binary part or an embedded workbook.
  - 3,000 slides that are one part.

  All were read and drawn, at most 238 MB.
- **The tools' arguments.**
  - The fuzz: 2,000 calls, no crash, hang or protocol error.
  - By hand ([calls/c1.json](2026-10-23-presentations-and-guards/calls/c1.json)):
    - degenerate regions: refused;
    - reversed corners: taken in order;
    - a zoom of 10⁹ or 10⁻⁶: kept within 800 % and the least;
    - slides 0, 2³¹ or 1.5: refused;
    - a show stepped past its end: "black", then ended;
    - `save_as` without a name or into a missing folder: refused;
    - an SVG or monochrome PDF of slides: refused, with the reason;
    - a duplicated slide list: written once.
- **The deck changed during a show** ([c18](2026-10-23-presentations-and-guards/calls/c18.json)):
  - Seven slides replaced by two while the show stood on slide 7: the show and tab followed; "next"
    went to the show's end.
  - A truncated file read again: refused ("Not a ZIP archive"), the old slides kept.
  - Closing a deck while its show played, and `play` twice: no crash.
- **A dialog from the right-click menu.** It is answered with the dialog's controls, and the next call
  says it waits for `set_dialog`.
- **The guards' file names.** `new_project` with `../`, `a/b`, `a\b`, an absolute path, `..`, a NUL or
  `con`: refused. Newlines in `rename` and `set_label`: refused.
- **Size and speed (Release).**

  | deck | slide drawn in | peak |
  |---|---|---|
  | 20,000 shapes on a slide (each with the theme's shadow) | 8 s | 982 MB |
  | 3,000 table rows | 0.36 s | |
  | rows of 2,000 cells | 0.8 s | |
  | 20,000 paragraphs | 0.9 s | |
  | 500,000 characters of text | 0.8 s | |
  | 100,000 characters in an autofit box | 0.27 s | |
  | a pie of a million slices | 9.7 s | |

  Exporting a 3,000-slide deck (500, B4) took 0.3 s.
- **`fuzz-sch.py`:** 400 mutants of the 253 examples, seed 2310, `--extra` with A1's schematic: 800 runs,
  no failure (Release).

## Reproducing

The scripts are in [2026-10-23-presentations-and-guards/](2026-10-23-presentations-and-guards/). Run them
with `python3 -B -I`, from a scratch folder.

- **The deck fuzzer.**
  - `fuzz_pptx.py make <seeds> <out> <count> <seed>` makes the mutants and lists each one's mutations
    in `mutants.txt`.
  - `fuzz_pptx.py run <test_pptx_doc> <out> <findings.txt> 25 400` draws them in batches of 25. A
    failure is pinned to its deck by the last `deck:` line, and the batch's log is kept beside the
    findings.
  - The runs here used seeds 1, 7 and 11. The seed decks were the reviewer's `viewer_test.pptx`, three
    decks from the workspace, and one from `~/Documents`, copied (run 1 used the first three).
  - The script is as run 3 left it: nesting at most 40 deep, and geometry mutations. Runs 1 and 2
    nested 50, 400 or 3,000 deep and had no geometry mutations.
- **The tools' fuzzer.** `fuzz_mcp.py <qucs-s> <home> <deck> <calls> <seed> <log>`, with `FUZZ_VALID=1`
  for mostly valid arguments (seeds 1 to 4; seed 4 on a deck whose layout is its own master).
- **Calls by hand.** `mcpcalls.py <qucs-s> <home> <calls.json> [files...]` runs a list of calls through
  the server, each with its time and the server's memory. `["__copy", {"from", "to"}]` copies a file
  over one in the workspace between calls. `NL='⏎'` shows the answers' line breaks. The lists used are
  in [calls/](2026-10-23-presentations-and-guards/calls/):

  | finding | list |
  |---|---|
  | A1 | `c8`, `c9`, `c10`, `c23` (its last call), `c31`, and `propscan.py` |
  | A7 | `c23` |
  | A6 | `c11` |
  | B1 | `c5` |
  | B2 | `c6` |
  | B4 | `c13`, `c14` |
  | N1 | `c16` |
  | N3 | `c2` |
- **Decks by hand.** Each writes into a folder named on its command line:
  - `mkprobe.py <deck> <out> <part> deepN|groupsN|ptcountN|altN`;
  - `mkcycle.py <deck> <dir>`;
  - `mkmedia.py <deck> <dir> <picture part>` (A3: `media_svg_use_expand`).

  `runone.py <test_pptx_doc> <deck> [seconds] [MB]` draws one of them with a time and memory limit.
- **A5's decks.** They alternate `<a:r><a:rPr b="1"/><a:t>x</a:t></a:r><a:r><a:rPr i="1"/><a:t>y</a:t></a:r>`
  in place of slide 2's first run.
