# The GUI while RF 2.5D solves

The user's report: "when the RF 2.5D is solving, it keeps refreshing the GUI and won't allow other windows such as
"save" window appear". Two things were wrong.

## What was found

**The waits never slept.** `RF25Doc`'s five waits were `waitForLayout`, `waitForPrepared`, `waitForSolved`,
`waitForSolvesToEnd` and `waitForAnalysis`. Claude's `rf25_*` tools use them, and so do the HFSS export and the tests.
Each was a loop of `QCoreApplication::processEvents(AllEvents, 20)`. That call returns as soon as nothing is queued, so
the loop asked again at once. The multiphysics tools wait on their job's end without a loop of their own
(`whenJobEnds`).
- **Measured:** a solve waited for 5 s took 4910 ms of the GUI thread's CPU on Cocoa, and 2366 ms offscreen.
- **Why that keeps the Save panel away:** a run loop that is only polled never goes to sleep. macOS commits a new
  window to the screen (Core Animation's transaction) just before the run loop sleeps, so the Save panel, or any other
  new window, could not appear until the wait ended.
- **When it happens:** for as long as a tool waits, which for `rf25_solve` is up to its timeout (600 s unless given).

**Each tick of the progress bar rebuilt the panel's form.** While a solve runs, a 400 ms timer updates the bar, and it
also emitted `solvedChanged()`.
- The RF 2.5D panel took that as a new solution. It deleted its whole settings form and built it again, 15 to 18 times
  in 8 s, whichever node was shown.
- That redrew the panel, closed an open drop-down list and lost the scroll position.
- The tab's notes were rewritten with `setPlainText` on every tick, which sent them back to their top.

## What changed

- **`RF25Doc::waitWhile(going, ms)`** (new, private) is what all five waits now use. It runs a `QEventLoop` that
  sleeps between events.
  - It checks every 20 ms, and also as soon as the model emits `stateChanged`, `preparedChanged` or `solvedChanged`.
  - It ends when `going()` is false or the time is up.
  - The waits' answers are what they were.
  - Its loop is now `misc::waitWhile`, shared with the Layout and 3D multiphysics waits
    (`2026-11-12-waits-that-sleep.md`).
- **`RF25Doc::progressed()`** (new signal) is what the 400 ms tick now emits instead of `solvedChanged()`. Nothing
  solved has changed when the count moves.
- **The panel** connects `progressed` to the new `RF25Panel::showProgress()`. While the Results node is shown,
  `showProgress()` sets the Results line ("Solving full-wave: 2 of 3 frequencies…") in place, and the form stays.
  `solvingSaid()` gives that line to both the form and the update.
- **The tracker** behind Background Jobs follows `progressed` as well as `solvedChanged`, so a solve's percent still
  moves.
- **The notes** are set only when their text has changed, and they keep their scroll position.

## Checked

- **New `aSolveWaitedForLeavesTheGuiThreadIdle`:**
  - A full-wave solve, held at its first frequency, is waited for for 1.5 s.
  - The GUI thread is busy under 20 % of that time, and another 50 ms timer ticks at least 15 times.
  - Once the solve is let go, the wait returns within 500 ms of the solve's end.
- **New `aSolveRunningLeavesThePanelsFormAlone`:**
  - With the Results node shown, the solve is let through one frequency at a time.
  - The Results line goes from 0 to 1 to 2 of 3, and the form and that label are the same widgets throughout.
  - The notes, scrolled down, stay scrolled down.
  - Once solved, the form is made again with what the solve found.
- Both pass offscreen and on the Cocoa platform. `aSolveShowsHowFarItHasGot`, the progress bar's own test, passes as it
  did.
- `breaks.py` undoes each piece. Its results are in `results.txt` beside it.

## Not checked

The native Save panel itself was not opened during a solve. The test binary never runs `[NSApp run]`, and there a
native panel does not come up even without a solve. What was measured is the cause: the GUI thread polling without
sleeping while a tool waits. The fix removes that polling.
