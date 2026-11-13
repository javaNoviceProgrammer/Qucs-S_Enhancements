# The Layout and 3D multiphysics waits, sleeping, and a 3D model closed without crashing

The follow-up to `2026-11-12-rf25-gui-during-solve.md` (the user: "Do this task here", then "Fix the 3D tab close crash
too"). Two other waits on the GUI thread polled the event queue the way RF 2.5D's had, and checking one of them on Cocoa
turned up a crash on closing a 3D model.
- **`LayoutDoc::waitForLoaded`** looped on `QCoreApplication::processEvents(AllEvents, 20)`. Claude's layout tools
  wait with it for a file being read, for up to 60 or 120 s.
- **`Multiphysics3DDoc::waitForIdle`** did the same with a 2 ms nap. Only the tests call it, because Claude's 3D tools
  already wait for the job's end without a loop of their own.

`processEvents(flags, ms)` returns as soon as nothing is queued, so these loops never let the run loop sleep. On macOS a
window opened meanwhile, such as a Save panel, cannot reach the screen until the wait ends.
- **Measured with the old 3D wait**, on Cocoa: a solve waited for 765 ms took 312 ms of the GUI thread's CPU.
- **With the new one:** 114 ms, most of it drawing the solution once the solve has ended.

## What changed

- **`misc::waitWhile(going, ms, wake)`** (new, `misc.{h,cpp}`) is RF 2.5D's sleeping wait, moved where every document
  can use it.
  - It runs a `QEventLoop` that sleeps between events and checks every 20 ms.
  - It also checks at once wherever `wake` connects a signal to the check it is given. The connections have the loop
    as their context, so they end with the wait.
  - It ends when `going()` is false or the time is up.
- **Each document wakes it on its own signal:**
  - `RF25Doc::waitWhile` passes `stateChanged`, `preparedChanged` and `solvedChanged`;
  - `LayoutDoc::waitForLoaded` passes `loaded`;
  - `Multiphysics3DDoc::waitForIdle` passes `stateChanged`.

  What each wait returns is unchanged.
- **`~Multiphysics3DDoc`** now disconnects every child's signals from the model before its widgets go.
  - The crash: in `QWidget`'s own destructor, the tab's page going sends the focus elsewhere.
  - The plot expression's line edit loses the focus and emits `editingFinished`.
  - Its slot (from `buildUi`) called `setPlotExpression` on the model, whose own part was already destroyed, and
    `Expression::compile` read through a destroyed member (SIGSEGV at 0x18, on Cocoa).
  - It was the old wait's crash too. ctest runs offscreen, where the focus does not move like this.
- **`LayoutDoc::holdReading(bool)`** (new, for tests) makes every layout read wait before it reads, until it is let go
  or cancelled, as `RF25Doc::holdPreparing` does for RF 2.5D's meshes. A read going on can then be seen in any build:
  a million shapes are read in 0.2 s here, too quickly to measure a wait on.

## Checked

- **How sleeping is measured.** The tests count the event dispatcher's `aboutToBlock` during a wait: how often the run
  loop went to sleep. A loop of `processEvents` never sleeps, and that is what keeps a new window off macOS's screen.
  GUI-thread CPU alone cannot tell them apart offscreen, where a loop napping 2 ms each turn costs as little as a wait
  that sleeps (30 ms against 35 ms for a 0.7 s solve).
- **New `test_layout_doc` `aLayoutWaitedForLeavesTheGuiThreadIdle`:**
  - A held read is waited for for 1.5 s.
  - The run loop sleeps at least 15 times, the GUI thread is busy under 20 % of the time, and another 50 ms timer ticks
    at least 15 times.
  - Once the read is let go, the wait returns within 500 ms of `loaded`.
- **New `test_multiphysics3d_doc` `aModelWaitedForLeavesTheGuiThreadIdle`:**
  - The shipped chip heater is solved and waited for, at least 300 ms to be judged.
  - The run loop sleeps at least once per 100 ms of the wait: 57 times offscreen and 94 to 109 on Cocoa.
  - The wait returns within 500 ms of the solve's end.
  - Its CPU is printed but not judged: the wait includes drawing the solution once solved, which takes 0.1 to 0.3 s
    on Cocoa.
- **New `test_multiphysics3d_doc` `closedWithTheFocusInThePlotField`:**
  - A solved model is closed.
  - When its widgets' `destroyed()` comes, none of its children is still connected to it. Before the fix, 23 were.
  - The plot field's `editingFinished` is then emitted, as the focus leaving it would, and it is harmless.
- RF 2.5D's `aSolveWaitedForLeavesTheGuiThreadIdle`, from the change before, now runs through `misc::waitWhile`.
- All three new tests pass offscreen and on Cocoa.
- `breaks.py` undoes each piece. Its results are in `results.txt` beside it.
