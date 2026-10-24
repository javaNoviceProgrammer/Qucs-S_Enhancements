# The CI-only failures of 23 and 24 October

Run 40038528347, on `dc49cc0`, was red in two jobs while every suite passed here:

- **macOS · Release** — `TestMcpWishlist::aModelTabFollowsItsFileOnDisk()`: a model whose file is written
  over was not read again.
- **Linux · Debug + ASan/UBSan** — `TestPptxDoc::picturesDrawn()` and `::claudesTools()`: the box drawn
  for an EMF picture, and what the slide says it did not draw.

Both came of the same kind of thing - a test that depended on something of the host rather than of
Qucs-S - and the first of them was hiding a real gap in how a document notices its file.

The next run, 40091386626 on `c998c2d`, showed the same macOS job red in **three** tests: the one above,
`aCallRightAfterAWriteSeesTheFile` - the same gap reached through the per-call read, which this working
copy cannot show but the verification worktree did - and `theModelAnswersSayWhatEachNumberIs`, one of the
previous round's own tests, for a reason of its own: see the last section.

## What each was

| | What happened | Why it passed here |
|---|---|---|
| **macOS** | `follows.qfem` (a copy of `coax.qfem`) was replaced by a copy of `coax_rlgc.qfem` and the tab kept the old model | `QFile::copy` carries the source's date (it clones the file on APFS), and in this checkout the two examples are stamped four hours apart, so the replacement looked newer. A fresh checkout stamps them alike |
| **Linux** | The EMF placeholder's middle pixel read dark, and the first "not drawn" note was not the EMF one | The placeholder's caption is drawn centred, and whether the middle pixel falls on a glyph is the host font's to decide; and `fontOf()` adds "the font Arial, not installed" to the same list - macOS has Arial, the runner has not |

## The gap behind the macOS one

`QucsDoc::changedOnDisk()` asked whether the file was **newer** than the date the document last read or
wrote. Two things follow from that, and both are real outside the tests:

- A file put back from a backup, copied with `cp -p`, checked out at an older revision or unpacked from
  an archive with its dates carries an **older** date. The watcher fires, Qucs-S looks, decides nothing
  has changed, and the tab keeps content that is no longer in the file - with nothing said.
- A checkout writes a whole tree in one moment. `git worktree add` here stamped `coax.qfem` and
  `coax_rlgc.qfem` **74 microseconds apart** - the same millisecond, which is all `QFileInfo` reports. The
  date then says nothing whatever, in either direction.

So the date alone cannot answer the question, and asking only "newer" answers it wrongly. What it is now:

```cpp
bool QucsDoc::changedOnDisk(const QDateTime& onDisk) const
{
  if (onDisk != a_lastSaved) return true;
  if (a_lastSize < 0 || a_DocName.isEmpty()) return false;
  const QFileInfo info(a_DocName);
  return info.exists() && info.size() != a_lastSize;
}
```

`a_lastSaved` was already the date the **file itself** had when the content was last read or written -
never a clock reading - at every load, so comparing for difference rather than for "newer" is exact. The
size is the second signal, for the case the date cannot express, and the two are now taken together in
one place, `stampedFromFile()`, so they cannot drift apart:

```cpp
void QucsDoc::stampedFromFile()
{
  const QFileInfo info(a_DocName);
  a_lastSaved = info.exists() ? info.lastModified() : QDateTime::currentDateTime();
  a_lastSize = info.exists() ? info.size() : -1;
}
```

Every load and save of every document kind calls it - schematics, text, spreadsheets, zip documents and
multiphysics models - in place of the nine assignments that set the date by hand. Two of those assignments
took the **clock's** reading just after writing the file (`MultiphysicsDoc::save()`, `SheetDoc::save()`),
which differs from the file's own date by a little; under the old "newer" rule that was harmless, under
"different" it would have made Qucs-S read back every file it had just written.

**When the stamp is taken matters as much as what it holds**, and two cases drew the line:

- *After a write* the file is Qucs-S's own work, so a stamp taken then is right. `SheetDoc::save()`
  seemed to get one from the `reread()` it calls, but `reread()` stamps nothing at all and gives up
  quietly on a file it cannot read again - the old clock line was the only stamp a save there ever had.
  Without it the document kept its pre-save stamp, and a second Save asked the user to write over their
  own save (`TestClaudeCode::theApplicationWorksWithTheDock`). It stamps in `save()` now, from the file.
- *Before a read* a stamp taken afterwards is wrong, and quietly so. A Markdown file rewritten while its
  text was still being set got stamped with the **new** file's date although the document held the old
  bytes, so the change was never read at all (`aFileWrittenWhileReadIsReadAgain`). Every load now passes
  the look of the file **as the bytes read were taken** - `stampedAs(asRead)`, captured before the read,
  which is what `Schematic::load()`'s own comment had been saying all along. Stamped older than the bytes
  held, the worst that follows is one more read; stamped newer, the change is lost.

## What the Linux pair was, and was not

Neither was a fault in the reader. `kind()` returns "EMF" and `drawable()` is false for that file on
every platform; the box is drawn the same way everywhere. What differs is the host:

- The caption inside the box is drawn centred in `Helvetica`, which CI's Linux has not got. The test read
  the box's exact middle - a coin toss between a glyph and the paper between two glyphs. It now reads four
  points inside the box and clear of the caption, which is what "a light box, not the picture" means.
- `fontOf()` notes a font this machine has not got in the same list as content Qucs-S cannot draw, and the
  title is laid out before the picture, so on a machine without Arial the font's note comes first. The
  tests asked for the head of the list; they now ask for the deck's own notes, with the host's filtered
  out (`ofTheDeck()`).

A new test, `whatTheDeckLacksAndWhatThisMachineLacks`, builds a deck whose theme font is one **no** machine
has, so both kinds of note are there on any host, in that order, and the box's corners are light. That is
CI's situation, reproducible here.

## Tested

| Test | Suite | What it holds to |
|---|---|---|
| `aModelReplacedByAFileDatedNoLaterIsStillRead` | `test_mcp_wishlist` | A model replaced by a file dated an hour older, then two hours older, then **to the same millisecond**, is read every time, and the document ends up stamped with the file's own date |
| `aModelTabFollowsItsFileOnDisk` | " | The date it means is now set outright rather than inherited from two example files' stamps |
| `aCallRightAfterAWriteSeesTheFile` | " | Unchanged - but it was failing in the verification worktree for the same reason, and passes now |
| `whatTheDeckLacksAndWhatThisMachineLacks` | `test_pptx_doc` | Both kinds of note, the font's first, the deck's own told apart from it, and the box light at its corners |
| `aSaveIsNotReadBackAsAnotherProgramsWrite` | " | A model edited, saved and left for 2.5 s - the watcher's 400 ms and the 1500 ms sweep - is not read back in: its revision is where the save left it, its stamp is the file's own date, and a file put there afterwards at that very date is still told from it |
| `picturesDrawn`, `claudesTools` | " | Read inside the box and clear of its caption; the deck's own notes rather than the head of the list |

Verified in a worktree of its own, because the checkout there stamps the two example files alike - the
very condition CI's runners produce and this working copy does not.

## Broken on purpose

`docs/feature_gaps/2026-10-24-ci-only-failures/breaks.py` holds six, run against that same worktree (it
takes `QUCS_SRC` and `QUCS_BUILD`, and its defaults point there, which matters: the checkout's identical
stamps are half of what is being tested). The reload asking "newer" again; the date deciding alone with
no size behind it - which catches two tests, the second of them `aCallRightAfterAWriteSeesTheFile`, the
per-call path the same gap reaches; a stamp taken without its size; a save stamping itself from the
clock; the box read outside itself; and the deck's notes not told from the machine's.

The first pass caught three of six, and the three it missed were worth more than the three it caught:

- **A save stamping itself from the clock was caught by nothing.** The rule - stamp from the file, not
  from the clock - had no test at all. The first one written for it watched for the self-reload such a
  stamp invites, and that turned out to be a race of a single millisecond (the write and the clock
  reading usually land in the same one), so it caught nothing either. What the test holds to now is the
  stamp itself: a save leaves its file's size as well as its date, and a file put there afterwards at
  that very date is still told from it.
- **Reading the box's middle again cannot fail here**, because whether the caption's glyphs cover that
  pixel is the host font's to decide and Helvetica leaves it clear on this machine. That is precisely why
  the test no longer reads it, so the break was replaced by one that asks that the points read are inside
  the box at all.
- **`picturesDrawn` cannot catch the filter being removed**, its deck having no text and so no font note.
  That break belongs to the new test alone.

## The third one: a point that was a point here and two numbers on CI

`theModelAnswersSayWhatEachNumberIs` built `fem_evaluate`'s `at` - a list of one point - as

```cpp
const QJsonArray at{QJsonArray{0.0005, 0.0}};
```

AppleClang 21, which builds this working copy, reads that as a list holding one list. AppleClang 17,
which builds CI's macOS job, reads it as a **copy** of the inner list - `[0.0005, 0]`, two numbers - and
is right to: [dcl.init.list]/3.2 takes a braced single element of the same type as copy-initialization,
before any initializer-list constructor is considered. So the call went out malformed and the tool
refused it, exactly as it should ("at[0] is a list, not the number 0.0005"), and the test read an empty
answer.

The project already knew this trap - `test_qucs_control` and `test_multiphysics_doc` each build a
one-element list by `append()` with a comment saying why - and the new test simply did not follow it. It
does now, and it checks the shape it built (`at.size() == 1`, `at.first().isArray()`), so a list that
flattens again says so at once instead of looking like a fault in the tool.

Nothing else in the tree is at risk: a braced list of **two or more** arrays cannot match the copy
constructor, and the only single-element ones left are the three that already build by append.

## Not done

- **A file replaced by one of the same date *and* the same size** is still invisible. Telling those apart
  means reading the file on every watcher event where both match, which is the common "nothing really
  changed" case; the cost is not worth it.
- **The two kinds of "not drawn" are still one list** in `read_pptx`'s answer and in the document's own
  line. A reader asking what a deck lacks gets this machine's missing fonts mixed in with content Qucs-S
  cannot draw. Separating them - "not drawn" for the deck, "fonts" for the host - would be a better answer
  and would make the ordering question moot; it changes the shape of a tool's answer, so it is proposed
  rather than done here.
- **`test_git_integration`'s "asked though the file's date says nothing"** now has a date that does say
  something (it is 60 s older, which is a change under the new rule). The assertion still holds - a file
  in conflict asks whatever the date says - but the case it meant to isolate can no longer be built from a
  date alone.
