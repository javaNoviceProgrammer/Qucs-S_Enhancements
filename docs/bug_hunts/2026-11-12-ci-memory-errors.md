# GitHub's CI on `main`: two memory errors and a compiler's reading

GitHub's CI on `main` has failed in every run that finished since 11
November. Two of the Linux job's failures (Debug, ASan/UBSan) were real
memory errors, both in `main` at `47bd72cb` and in the v27.1.3 release made
from `3518dcd9`. (M1 was fixed on `main` in the meantime, in `0e5a506d`; M2
and M3 are fixed here, on `6b600de6`.) One macOS failure came from that runner's compiler only.
The other failures (timeouts on GitHub's slower runners, Linux-only
numerical differences) are another task's and are not covered here.

Read from the failed logs of runs 43246049462 (`f172b12c`) and 43152530696
(`3518dcd9`). Reproduced and checked on a local ASan/UBSan Debug build
(Apple clang, `-fsanitize=address,undefined`) and a Release build.

## M1 - use after free in `RF25Doc::drawPicked` (the Via draw tool)

`rf25doc_draw.cpp`, the Via tool's click:

```cpp
const Layer* chosen = a_model.stack().named(layer);
...
if (chosen != nullptr && chosen->kind == Layer::Via) ...
```

`Model::stack()` returns a `Stack` by value, and `Stack::named` returns a
pointer into that copy's `layers`. The copy is freed at the end of the
statement, so `chosen->kind` reads freed memory. ASan: heap-use-after-free
in `RF25Doc::drawPicked`, freed by `Stack::~Stack` on the line before, from
`test_rf25_doc aModelsShapesAreDrawnOnItsTab` (the Via tool's click). In
Release it read whatever was left there: a via drawn on the wrong layer, or
none, depending on the heap. It came with `8a95e4e6` (merged in `5683d3ff`,
11 November).

**Fix:** the `Stack` kept in a local, as the rest of the file does. This
fix was made here too, and was also found and committed on `main` by
another session (`0e5a506d`, 12 November) before this one was merged, so
`main`'s version is kept. This commit adds the CI change below.

**The same pattern elsewhere.** Every `stack()` in the sources was read for
a pointer or reference into the copy kept past its full expression:

- `rf25doc_draw.cpp`'s `a_model.stack().named(to) == nullptr` only compares:
  fine.
- `rf25panel.cpp`'s `for (const Layer& layer : a_doc->model().stack().layers)`:
  fine. The range binds to a member of the temporary, which extends the
  whole temporary's life.
- `test_rf25_stack.cpp` (`whatNeitherFileSaysOfAMetalIsKeptByName`): the
  same bug twice, `const Layer* metal = back.stack().named(...)` and
  `const Layer* other = fresh.stack().named(metal->name)`, both read on the
  next lines. CI's Linux job had left the suite out for this since
  11 November. `0e5a506d` fixed it the same way. This commit puts the
  suite back in that job's list (`.github/workflows/ci.yml`).
- The other uses (`QCOMPARE(x.stack().named(...)->kind, ...)` and the like)
  use the pointer inside the one expression: fine.

**Test:** `test_rf25_doc aModelsShapesAreDrawnOnItsTab`, which already clicks
the Via tool, fails under ASan on the code before the fix (heap-use-after-free
at `rf25doc_draw.cpp:201`) and passes after it.
`test_rf25_stack whatNeitherFileSaysOfAMetalIsKeptByName` does the same
(heap-use-after-free at `test_rf25_stack.cpp:653`).

## M2 - a read before the start of a graph's screen points in `Diagram::calcData`

`diagrams/diagram.cpp`, the solid, dashed and dotted styles, at a missing
value (NaN, a gap):

```cpp
if (p - g->begin() >= 2 && (p - 2)->isStrokeEnd() && !(p - 2)->isBranchEnd() && !(p - 1)->isStrokeEnd()) p -= 2;
if (!(p - 1)->isStrokeEnd()) (p++)->setStrokeEnd();
```

The buffer starts with a stroke's end. When a curve's first drawn value
stands alone before a missing value (1, NaN, ...; or NaN, 1, NaN, ...), the
first line drops the lone point and the stroke's end before it, `p -= 2`.
That stroke's end is the buffer's first entry, so `p` lands on
`g->begin()`, and the second line reads `(p - 1)`, one entry before the
buffer. ASan reported a heap-buffer-overflow 24 bytes before the `ScrPt`
buffer, in `Graph::ScrPt::isStrokeEnd`, from `test_data_import
aDatasetIsWrittenBesideTheSchematic` (a dataset of 1, NaN, 3). Without ASan
the result depended on those bytes: either the stroke's end was written
again, or the curve began with no stroke's end at all. It came with
`e32067a1` (the NaN gaps, merged in `3b448e5c`, 11 November).

**Fix:** only the lone point is dropped (`--p`), and the stroke's end before
it stays. Everywhere else this is the same as before: the old code dropped
both and then wrote the stroke's end again, because the entry before it is
never a stroke's end. At the buffer's start the sentinel is no longer
touched. An assertion keeps `p` past the first entry before the look-back.

**The other look-backs.** The other `(p - 1)`, `(p - 2)` and `(p - 3)` reads
were checked for the same start-of-buffer case:

- The branch's end in `calcData` (`filled >= 2` before `(p - 2)`) is
  guarded. Its `p -= 2` can reach `g->begin()`, but nothing is read before
  the buffer after it.
- `rectClip` and `clip` run only after two points of one stroke, so
  `p - 3` is at least the first entry.
- `Rect3DDiagram::calcData` starts one past its own stroke's end and only
  moves forward, so `(p - 1)` is always inside. Its `Mem` buffer's
  `(p - 1)` starts from `Mem + 2`.
- `PoleZeroDiagram`, `StackedDiagram` and `NicholsDiagram` call
  `RectDiagram::calcData`, which is `Diagram::calcData`.

**Test:** `test_graph_style aLoneValueBetweenGapsIsNoStroke`, four rows (the
first value alone, the first value then a gap of two, the second value alone
after a gap, a lone value in the middle). Each checks the screen points
(a stroke's end first, then each stroke's points) and the drawing. The
first three fail under ASan on the code before the fix. The last checks
that the middle of a curve is drawn as before. `test_data_import` passes
under ASan.

## M3 - `QJsonArray{row(...)}`, a copy to one compiler and a list to another

`test_rf25_doc.cpp`, `aModelsShapesAreDrawnOnItsTab`:

```cpp
QCOMPARE(rows, QJsonArray{row({QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("M1")})});
```

`row()` returns a `QJsonArray`. A braced list of one element of the class
itself is read as a copy by GitHub's macOS compiler (`["1","0","M1"]`), and
as a list of one element by GCC and the local Apple clang
(`[["1","0","M1"]]`). So the test failed only on GitHub's macOS job.
`scripts/ci/check-json-nesting.py` catches `QJsonArray{QJsonArray{...}}`,
but not a call there.

**Fix:** `QJsonArray{QJsonValue(row({...}))}`. The element is a
`QJsonValue`, so no compiler reads it as a copy.

**The lint:** `check-json-nesting` now also flags a braced `QJsonArray`,
`QStringList` or `QVariantList` whose one element is a call that gives the
list's own class. That means a function the file declares as returning it
(or, for names the file does not declare, one whose every declaration in
the sources does), or Qt's `toArray()`, `toStringList()`, `split()`,
`keys()` and `toList()`. Literals are blanked first, so a brace or comma in
a string is not read, and the list may span lines. It flagged this line and
nothing else in the sources. An overloaded `toJson` (a `QJsonArray` of
problems, a `QJsonObject` of everything else) is not flagged. It runs in
about two seconds.

**Test:** the lint (`check_json_nesting` in ctest, and CI's first step)
fails on the line before the fix and passes after it. The test itself
cannot fail locally, because the local compiler reads it as a list either
way.

## Checked

- **Before the fixes**, on `47bd72cb` under ASan/UBSan through ctest, one
  test function at a time:
  - `test_rf25_doc aModelsShapesAreDrawnOnItsTab`: heap-use-after-free at
    `rf25doc_draw.cpp:201`.
  - `test_rf25_stack whatNeitherFileSaysOfAMetalIsKeptByName`:
    heap-use-after-free at `test_rf25_stack.cpp:653`.
  - `test_data_import aDatasetIsWrittenBesideTheSchematic`:
    heap-buffer-overflow in `Graph::ScrPt::isStrokeEnd`.
  - `test_graph_style aLoneValueBetweenGapsIsNoStroke`: its first three rows
    give the same heap-buffer-overflow. "in the middle" passes.
  - `check-json-nesting`: flags `test_rf25_doc.cpp`'s line.

  All of these are CI's own reports.
- **After the fixes**, rebased onto `6b600de6`:
  - The whole Release suite through ctest (`-j4`): 199 of 199 passed,
    `check_json_nesting` among them.
  - Under ASan/UBSan: `test_rf25_doc` (whole, 803 s), `test_graph_style`,
    `test_data_import`, `test_rf25_stack` and `check_json_nesting` passed,
    with no sanitizer report.
- **Not checked here:** Linux, and GitHub's macOS compiler. The macOS line
  is fixed by construction (a `QJsonValue` element). On GitHub's run of
  `48fe4625` it still failed as before, at `test_rf25_doc.cpp:2471`.
