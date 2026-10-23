# The second re-check: a region drawn by hand, and two things to look at

A re-check of `abefdd2` (`~/Desktop/qucs-reviews/2026-10-23-claude-mcp-wishlist-recheck2.md`) found the
mesh work and the script message right, and left three things: one real failure, one piece of fallout
worth knowing, and one report I could not reproduce.

| # | Item | Now |
|---|---|---|
| **1** | `read_pdf`'s `region` worked for a selection's own boxes but fell back to the whole page for a region drawn by hand - any that starts in the margin | The two points are moved onto the text: the first and last run the region meets, in reading order. All four of the re-check's failing regions now read |
| **2** | A model run through `fem_converge` between `472a813` and `abefdd2` and then **saved** may carry divided Size features, from the compounding bug fixed in `abefdd2` | Nothing in the repository was touched (no `.qfem` of `examples/` differs from git), so this is only about models saved outside it. The write-up says how to spot one |
| **3** | A `.qfem` tab was said not to reload when its file changed on disk | It does reload, here, and the conversation is told. There is now a test that holds it to that, and the one case where a reload is refused says so in the answer |

## What each change is

**1, the corners moved onto the text** (`qucscontrol_design.cpp`). Qt reads the text between two points
by finding the character nearest each of them. A region's own corners are usually in the margin - that is
what a region drawn by hand, or given as fractions, looks like - and near no character at all, so nothing
came back and the answer fell through to the whole page. The re-check's own suggestion is what it does
now: before reading, the first and last run of the page that the region meets (in reading order) give the
two points, a pixel inside each. The four regions it listed as failing all read:

| region (page 2 of the paper) | before | now |
|---|---|---|
| `[117, 343, 495, 376]` (a selection's boxes) | the 3 lines | the 3 lines |
| `[100, 600, 495, 660]` | whole page | the equation block |
| `[60, 343, 550, 376]` | whole page | the 3 lines |
| `[0.1, 0.4, 0.9, 0.45]` (fractions) | whole page | the lines there |

A region that meets no run at all now says "nothing: no text of the page is in it" rather than handing
back the page.

**2, the fallout of the compounding bug** (nothing to change). Between `472a813` and `abefdd2`,
`fem_converge` divided a model's local Size features by each factor in turn (so ×2 then ×4 left them at
a eighth) and put back only the mesh node's own properties, so a model **saved** while that state was in
memory kept the divided features. Nothing in the repository is affected: no `.qfem` under `examples/`
differs from git. A model of your own that was run through `fem_converge` that afternoon and saved
afterwards may carry them - a Size feature at an odd fraction of a round number (0.000125 where 0.001 was
meant) is the sign, and git or a backup is the cure. From `abefdd2` on, the node and its features are
copied before the run and put back whole, which is what the test of that round holds to.

**3, a model tab and its file** (a test, no change). I could not reproduce the report. With a `.qfem`
open and another model copied over its file, the tab reads the new one: `MultiphysicsDoc` is among the
kinds `watchDocuments()` follows and among those `reloadDocument()` reads again, the geometry and mesh
are invalidated by `applyModel()`, and the conversation is told with its next answer ("…was changed by
its file, changed on disk … and loaded again"). A probe through the real server shows the objects change
from the old model's to the new one's across a copy.

There is one case where a reload is refused, by design: a tab with **unsaved changes** keeps them, and
says so both in the status bar and to every conversation ("it has unsaved changes in Qucs-S, so the
window shows those, not the file"). That is the likeliest explanation of what the re-check saw, with the
other being that the file on disk at that moment was still the compounded copy of item 2, which was
restored only later.

What this round leaves behind is a test, since none held the behaviour before:
`aModelTabFollowsItsFileOnDisk` opens a model, writes another over its file, and waits for the tab to
show the new one.

## Tests

`test_mcp_wishlist` gains a function and one is extended; it has 18.

| test | what it holds to |
|---|---|
| `aModelTabFollowsItsFileOnDisk` | a model opened from `coax.qfem` shows `coax_rlgc.qfem`'s three objects once that is written over its file, at a higher revision, with no unsaved changes, and the tools see the new model |
| `aRegionKeepsThePdfsLinesAndOneLineEnding` (extended) | a region that starts in the margin of a page whose boxes cannot be paired reads its text, and less of it than the whole page; one that meets no run says "nothing" and does not fall back to the page |

## Broken on purpose

`breaks.py` gains 5 (S1, S3) and holds 85, all caught. One R1 break was re-aimed at the text this round
replaced, and one was **dropped as unreachable**: with the corners moved onto the runs, the last resort -
the whole page, where runs are found but reading between them still answers nothing - is not reached by
any PDF the suite can write. Two of the three ways a region can be read are tested; that one is a guard
with no fixture behind it, and the write-up would rather say so than carry a break that passes for the
wrong reason.

Two went uncaught at first, both because this round changed which way the fixtures take: the margin test
only asked that something came back (the whole page is something), and it now asks that less than the
page came back.

## Not done

- **A region meeting one short run.** Where the only run a region meets is a page number in the foot,
  Qt's reading between two points on it came back with the page's whole text rather than that run. The
  precise way (boxes grouped into lines) does not have this, and a region over prose does not meet it.
- **The precise way on a paper.** Unchanged from the last round: 201 boxes for 42 lines, 93 after
  grouping, so a paper's page still takes the second way.
