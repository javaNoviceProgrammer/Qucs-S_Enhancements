# Bug hunt: the Claude Code dock, fuzzed whole

*3 November 2026 - Qucs-S at `fcd2457` (the Release build; the ASan/UBSan build where said), in a
worktree of its own while Multiphysics 3D was built in another. In the foreground. Everything found
is fixed in the commit that adds this report, each fix with a test and broken in turn
([breaks.py](2026-11-03-claude-dock-fuzz/breaks.py)).*

The user's request: *"focus on claude chat GUI and run fuzz tests on it to make sure it has no issues
and glitches"* - the same words as on 22 October, so this hunt starts where
[that one](2026-10-22-claude-panel-fuzz.md) stopped. Its fuzzer drives one panel. Since then the dock
has gained tabs, the held calls of auto mode, the guards and the Extras menu, pins and the pen's
drawings, Open in Viewer, resume and the conversations kept.

## The fuzzers

**`fuzzThePanel`**, the fuzzer of 22 October, unchanged but for its report of a part's start: it gave
"from part 0" whatever the part was, since it read the start after drawing all afresh.

**`fuzzTheDock`** (new, `qucs/tests/test_claude_fuzz.cpp`) drives a whole `ClaudeCodeTabs`. Its
sessions run a program that keeps what the dock writes to it, each in a file of its own. It has a tool
host (`FuzzHost`) whose tools do different things:
- some only look, some are allowed in auto mode;
- one cannot be undone, one is asked about each time;
- one asks the user first, one answers later, one answers with an image, one with 300,000 characters;
- some fail; when cancelled, it stops some calls and leaves others to end in their time.

**The program's side.**
- The 22 October messages.
- Claude Code's MCP traffic for the dock's tools: calls with and without a tool use's id, the
  handshake, answers to the server's questions, cancellations of calls a moment old.
- Permission requests for the host's tools, uses of them and their results, turns ending.

**The user's side.**
- Tabs: opened, closed (asked or not), shown, renamed through their editor (Enter, Esc, a click
  elsewhere, the tab closed mid-rename) and through their menu.
- Conversations kept and brought back, `/resume`'s list.
- Every action of every menu of the panel: modes, models, the guards, pins, usage, exports, folders.
- The transcript's own menu, at a picture for Open in Viewer.
- The composer's keys, pastes and drops of files, images, text and HTML, on the composer and on the
  transcript.
- The document in front and what the pen drew on it, pins, a document renamed.
- The dock resized down to 40 pixels, hidden, its theme and font changed (8 to 21 points).

**What it checks.**
- After each step: as many tabs as conversations; no tab's title over a line; the dock's least width
  bounded (in proportion to the font), with the widest widget named when it is not.
- At each render, what `fuzzThePanel` checks: time, the newest plain reply shown, no formula's mark,
  the end followed, drawn in part as drawn whole.
- At the end: each of the program's requests answered once at most, and every line the dock wrote
  JSON.
- `QUCS_FUZZ_DUMP=<step>` prints the composer as it is at a step.

**Runs.**

| | Release | ASan/UBSan |
|---|---|---|
| `fuzzThePanel` before any change (main's binaries) | 40 seeds × 1,500 steps: clean | 6 × 294 s (about 24,000 steps each): clean |
| `fuzzTheDock`, first run | 37 of 40 seeds with findings | |
| both, after the fixes | dock 80 × 3,000 and 40 × 3,000 with cancellations, panel 40 × 1,500: clean | dock 6 × 280 s, panel 2 × 280 s: clean |

The six ASan runs of 22 October's fuzzer "failed" at 300 s with QtTest's watchdog, not a finding;
the run script now sets `QTEST_FUNCTION_TIMEOUT`.

## Summary

| | Finding | Fixed |
|---|---|---|
| **W1** | A word longer than a line - a path, a URL, another server's tool's name - in the permission card's title or subject, or in an MCP server's question, made the dock as wide as the word at its least: up to 8,741 pixels | `WrappedLabel` |
| **W2** | The permission card's four buttons side by side, whole: 727 pixels at the least | the two long ones cut where narrow |
| **W3** | A question's choices as whole buttons, a form's long field title, a choice box as wide as its longest choice | cut, wrapped, a few characters |
| **W4** | Images attached to a prompt side by side: six of them some 720 pixels | a row that scrolls |
| **W5** | The git bar with a long folder and branch: 600 pixels at the least, 363 without it | `ElidedLabel` |
| **W6** | A tab not in front kept the width a card once gave it (a stacked widget counts every page, and a hidden one is not laid out again) | by W1-W5 |
| **C1** | A crash: files that are not images dropped twice on the composer (Release and ASan) | the panel puts drops in itself, a file as its path |
| **A1** | A reply's image a scroll bar too wide (18 pixels) after a render that drew all afresh | fitted from the transcript's width |
| **A2** | A prompt's thumbnail 179.92 pixels high, not 180, once it had been narrower | its author's height kept |
| **N1** | `QFormLayout::takeAt: Invalid index 0` at every question card put away | its rows counted |

No other crash, hang, sanitizer report, request answered twice, line written that is not JSON, or
slow render was found. The slowest render was 138 ms in Release and 119 ms on ASan.

## W1-W6: long texts widened the dock and the window

A word-wrapped `QLabel` breaks lines between words only. It asks its layout for the width of its
longest word, and the layouts pass that on to the dock and to the window. The labels of 2 November
(`ElidedLabel`, 6b2e3d7) cut lines of one line. The cards' labels wrap, so they were left as they
were, and the dock's audit then never showed a card.

- **W1.** Permission requests carry such words in real use:
  - a file's path in a Write's or an Edit's subject;
  - a command in a Bash's subject;
  - "Claude wants to use mcp__github-enterprise-server__create_or_update_pull_request_review_comment_reply";
  - Qucs-S's own question before writing over a file names the file.

  `WrappedLabel` (`elidedlabel.{h,cpp}`) asks for eight characters' width. It breaks a word too long
  for its line where it must (`QTextOption::WrapAtWordBoundaryOrAnywhere`). A text whose words fit
  is drawn by `QLabel` as before, its selection with it. `text()` stays whole for the tests and for
  `get_ui`. Its size hint stops at forty characters: a dock never sized takes its contents' hints. The
  cards' titles, the subject and a form's field titles use it.
- **W2.** "Allow All Edits" and "Allow Qucs-S Control" are `ElidedToolButton`s, text-only. A tool
  button's default style is icon-only, which `ElidedToolButton` reads as no text to cut.
- **W3.** A question's buttons are cut where narrow, whole in their tool tips.
  - A choice box asks for eight characters' width. With `setMinimumContentsLength`, its least width
    counts none of its choices.
  - `AdjustToMinimumContentsLengthWithIcon` keeps its preferred width the same. By default it takes
    its longest choice once shown: 528 pixels at the least here.
- **W4.** The pictures are in a row of their own inside a scroll area, as tall as they are.
- **W5.** The git bar's folder and branch were cut to 200 and 220 pixels beforehand, then laid side
  by side with the stat and the pull request's button. They are `ElidedLabel`s now, whole in
  `text()`, as wide as before at the most.

The tests are `longTextsLeaveTheDockNarrow` (test_claude_code) and `longNamesLeaveTheDockNarrow`
(test_claude_git). The first gives every long text above, six images and four buttons. It checks:
- the panel's least width unchanged, and its preferred width;
- the subject wrapped, ink from its first line to its last;
- the buttons cut when the panel is at its least, their text whole;
- an empty form put away with no warning (N1).

## C1: a crash dropping files on the composer

Seed 300 of `fuzzTheDock`, step 217, crashed in Release and on ASan:

```
QTextLine::naturalTextRect() const
QPlainTextEditPrivate::ensureVisible(int, bool, bool)
QWidgetTextControl::insertFromMimeData(QMimeData const*)
QWidgetTextControlPrivate::dropEvent(...)
QPlainTextEdit::dropEvent(QDropEvent*)
```

The panel takes an image dropped on the composer. Anything else - a schematic dragged from the
Finder, a folder - went to `QPlainTextEdit`, which puts its URL in as text (`QMimeData::text()` of
URLs alone). The composer was empty, at an 18-point font, and the same folder was dropped twice.

Qt's drop inserts inside an edit block, and its own `insertFromMimeData` makes the cursor visible
before that block ends. The block's layout is then still the one from before the insertion, and the
line for the cursor's new place is not there (`Q_ASSERT(line.isValid())` in a debug Qt).

That is the reading of Qt's code that fits the trace, not something proved. Neither a test of the
same two drops nor one at the same font reproduces it: it needs the fuzz's exact geometry. The fix
was checked against seed 300 itself.

**Fix.** The panel takes every drop on the composer and puts its text in where it was dropped,
through a `QTextCursor` with no edit block around it. A file goes in as its path in the system's
form, one a line, rather than as a `file://` URL. A link stays as it is, and text as it is.

**Test:** `filesDroppedOnTheComposerAreTheirPaths` - three drops, then text.

## A1, A2: images drawn otherwise in part and whole

- **A1.** `fitImages()` subtracted the scroll bar's width unless the bar was visible. In a render
  that draws all afresh, the bar can already be visible while the viewport has not been narrowed yet,
  and the image came out the bar's width too wide (1046 against 1028). It is fitted from the
  transcript's own width now (`contentsRect()`), which a bar coming or going does not change. Found by
  three seeds (1051 in Release, 402 and 405 on ASan).
- **A2.** A prompt's thumbnail is put in at whole pixels (96 × 180). Fitted narrower and then back
  to its width, it took its own size's proportion, 179.921 pixels. `fitImages()` now keeps the
  height its author gave where the image is at its author's width (`kAuthorHeight`, beside
  `kOwnWidth`).

Both are caught when undone:
- **A1** by the dock's fuzz, through its check of the transcript drawn in part against drawn whole.
  The seeds that first found it replay other steps since the fuzz changed, so the break runs up to
  160. Seed 105 caught it.
- **A2** by `aGivenHeightIsKeptAtItsWidth` (test_markdown_doc): `fitImages()` alone, a thumbnail
  fitted narrower and back.

## N1: a warning at every question put away

`showNextAsk()` emptied each inner layout with `takeAt(0)` until it gave none. On a `QFormLayout`
the last `takeAt(0)` warns. It counts the rows now.

## The fuzzer's own findings

These were corrected in the fuzzer, not in Qucs-S:
- **A reply expected in a conversation no longer in front.** A rename step's last choice opens a
  tab, and a tab's menu may close one. The expected reply now belongs to its panel.
- **"A line not JSON".** Two programs appended to one file, and a 300 KB answer interleaved. Each
  program now writes its own file.
- **An image's size cache compared.** `kOwnWidth` and the rest are kept only once an image has been
  fitted, so they differ between drawn in part and drawn whole. They are left out of the comparison.
- **The width bound with the font.** A 21-point font widens everything. The bound now grows with the
  font's average character.

## Breaks

[breaks.py](2026-11-03-claude-dock-fuzz/breaks.py) has 35 breaks: these fixes, and the cancellation
of tool calls ([the MCP write-up](../feature_gaps/2026-11-03-claude-mcp-cancel.md)). Each was undone,
built and its tests run. All 35 are caught: 27 on the first run, the other eight once their tests
were made sharper. Four of those eight are this hunt's:
- **Widths measured after the queued shows.** Widgets put into a card already shown are shown by a
  queued call, and a hidden widget takes no room. So a form's plain field title, 1,388 pixels, had
  passed.
- **The choice box's break** undoes both of its lines. `setMinimumContentsLength` alone keeps its
  least width.
- **The subject's height** is compared with its text laid out as it is drawn. A `QLabel`'s height for
  the path was 75 pixels against 105, enough for a bound by characters per line.
- **The image fitted to the bar:** the fuzz tries 160 seeds, and seed 105 caught it.

The other four are the cancellation's; that write-up says how they were made sharper. The thumbnail's
break was caught at first by the fuzz's seed 28, but not on a later run. It has a test of its own now:
the new `fitImages()` one.

## Checked and found right

Under the fuzz, in 80 seeds of 3,000 steps in Release and six runs of 280 s on ASan, none of these
did wrong:
- held calls answered, refused, or withdrawn at a turn's end;
- calls sent to the background;
- MCP questions asked and answered, or cancelled when unanswered;
- restarts on mode changes;
- tabs closed with a call waiting, a rename under way, or a held call;
- conversations kept and brought back, the resume list;
- every menu action, the export dialogs (closed by the watch), Open in Viewer;
- pastes of images and text, keys with the command list shown;
- the theme and font changed mid-stream.

No request of the program's was answered twice. Every line the dock wrote was JSON.

## Reproducing

```bash
# a long run of either function, a seed of its own (the dock's trace is <trace>.dock)
QT_QPA_PLATFORM=offscreen QUCS_CLAUDE=/nonexistent/claude QTEST_FUNCTION_TIMEOUT=3600000 \
  QUCS_FUZZ_SEED=7 QUCS_FUZZ_SECONDS=280 QUCS_FUZZ_TRACE=/tmp/trace.txt \
  build/qucs/tests/test_claude_fuzz fuzzTheDock
```

`QUCS_FUZZ_STEPS` sets a pass of so many steps (400 by default, CI's). `QUCS_FUZZ_DUMP=<step>` prints
the composer then. [breaks.py](2026-11-03-claude-dock-fuzz/breaks.py) undoes each fix and runs its
tests, the cancellation of tool calls with them
([the MCP write-up](../feature_gaps/2026-11-03-claude-mcp-cancel.md)).
