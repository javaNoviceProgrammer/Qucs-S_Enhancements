# Bug hunt: `test_git_integration`'s worktree failures and crash under load

*13 November 2026 - Qucs-S on `claude/nice-volhard-c8cbb5` (on `48fe4625`), a Release build, macOS
27.2, Qt 6.12. Started from two full clean checks of `main`, both running six suites at a time. At
`469ebb7b`, `aWorktreeAsTheRecheckFoundIt` failed at its `trigger_action` with "“Making the worktree”
is open in Qucs-S and waits for an answer", and the next function, `claudeIsToldALookWasNotGuarded`,
crashed 1.3 s in (SIGSEGV reading address 0 in `QCoreApplication::processEvents`, under
`QTest::qWait`). At `373a9cc5`, the same message failed `aWorktreeAsTheCheckFoundIt`,
`aWorktreeAsTheRecheckFoundIt` and `claudesGitTools`, which makes no worktree at all. Run alone, the
suite passed. Found, fixed and tested here.*

## Summary

| | Finding | Where | Fixed |
|---|---|---|---|
| **F1** | A git job's progress window could be opened by the previous job's timer and then never closed. `runJob` starts a 600 ms timer, owned by `Commands`, that opens the window if `a_job == job` and the job is still running, comparing raw pointers. If a job finished and was freed within 600 ms, and the next job was allocated at the same address (macOS's allocator hands back the block just freed) and was still running when that timer fired, the first job's timer took the second job for its own. It opened a window-modal window under the first job's title and stored it in the first job's state, which nothing deletes. The second job's finish closes only its own window. The stray window stays up for good: its Cancel is tied to a job that is gone, it blocks the main window, and every Claude tool that is not a read-only one is refused while it is up | `gitui.cpp`, `Commands::runJob` | yes: the timer is owned by the job, so it is cancelled when the job is freed. At the job's end its window is also hidden at once, not only `deleteLater()`ed |
| **F2** | The crash. `aWorktreeAsTheRecheckFoundIt` drove the New Worktree window from `QTimer::singleShot(800, this, [&] {...})`: owned by the test object, and capturing the function's locals (`values`, `cancelled`) by reference. When the function returned early on F1's refusal, the timer survived it and fired in the next function, calling `set_dialog` and writing into stack memory that by then belonged to other frames | `test_git_integration.cpp` | yes: the window is answered by an answerer that waits for it (up to a minute, not a fixed 800 ms) and is removed when the function ends |
| **F3** | Nothing stopped one function's leftovers reaching the next. The answer helpers' timers (on `qApp`, alive up to 10 s, often capturing a function's locals by reference), a running job, and a window waiting for an answer all carried over. A leftover failed or crashed a later function instead of the one that left it | `test_git_integration.cpp` | yes: a `cleanup()` slot removes the function's answerers, waits for its git job, and fails the function if it left a window waiting for an answer (closing it, so that the next function starts clean) |

The parent session's guess, that the leftover job or window used something already freed (as when a
user closes a project or window while a worktree is being made), did not hold. The job's finish
callback captures `Commands`, which lives until the application quits, plus copies of strings. The
window is held through a `QPointer`, and its parent window deletes it if it closes first. In the
first run below, the stray window stayed up through `claudeIsToldALookWasNotGuarded` and that
function passed. What the user can reach is F1: a stuck progress window. It needs two git jobs
started back to back, the first one done within 600 ms (Fetch then Pull, say, or two
`trigger_action` calls from Claude).

## How it was found

The suite passed with a `QUCS_GIT` wrapper that made every `git worktree add` take 2 s, and again
with only the second of `aWorktreeAsTheRecheckFoundIt`'s worktrees ("taken") slowed. A slow job does
show its window, and the window goes away when the job ends. So "a job still running" did not explain
a window that outlived its job: `made()` had seen `runningJob()` go null before the failing call.
That points at a window that no job owns. Reading `runJob` again turned up the raw-pointer comparison
in the 600 ms timer.

The new test `aJobsProgressWindowIsItsOwn` sets up F1 directly. It runs a fast job (`rev-parse HEAD`),
waits until the job is over and freed, then starts a job through a wrapper that sleeps 1 s first. It
checks that while the second job runs, no window with the first job's title shows and at most one
progress window does, and that none is left afterwards. On `48fe4625`'s `gitui.cpp` it failed in its
first round: a window titled "First" came up during the second job. Before `cleanup()` existed, that
window was still open in `claudesGitTools` three functions later, and `git_stage` was refused with
"“First” is open in Qucs-S and waits for an answer". That is the `373a9cc5` failure.

To test the crash explanation, a temporary hook (since removed) opened a window-modal "Making the
worktree" window just before the recheck's `trigger_action`, standing in for F1's stray window. On
the old test code this reproduced the clean check exactly: the same refusal at the same line, then a
SIGSEGV reading address 0 in `claudeIsToldALookWasNotGuarded`, 0.86 s in. The macOS crash report's
stack matched the reported one: `processEvents` under `QTest::qWait` under that function. With F2 and
F3 fixed, the same hook fails only `aWorktreeAsTheRecheckFoundIt` (the refusal, then `cleanup()`'s
"left waiting for an answer: Making the worktree"), and the other 50 functions pass with no crash.

## The fix

- `Commands::runJob`: `QTimer::singleShot(600, job, ...)` in place of `(600, this, ...)`, so the
  timer is cancelled when its job is freed and can never see another job at the same address. At the
  job's end its window is hidden before `deleteLater()`, so it stops waiting for an answer right away,
  not when the event loop next handles deferred deletes.
- The test's answer helpers (`answerBox`, `answerInput`, `onWorktreeWindow`, `answerFolder`) hang
  their timers on one `answerers()` object, which `cleanup()` empties after each function.
  `onWorktreeWindow` waits up to a minute for its window, not 10 s.
- `cleanup()` (new): it removes the answerers, waits up to 120 s for a running git job, deletes
  deferred objects, and fails the function if a window is still waiting for an answer, after closing
  it.
- `aWorktreeAsTheRecheckFoundIt`: the `set_dialog` calls go through `onWorktreeWindow`, so they run
  once the window is up, not at a fixed 800 ms. The `trigger_action` gets 120 s, not 20. The worktree
  tests' waits for their job (`made()`, and the File Browser's worktree) get 120 s, not 30.

## Tests

- `test_git_integration aJobsProgressWindowIsItsOwn` (new): four rounds of a fast job followed by a
  slow one. It fails on the old `gitui.cpp` and passes with the fix. It logs how often the second job
  got the first job's address: 4 of 4 in each run here, so on macOS the reuse is the rule, not the
  exception. The fix does not depend on the address being reused, so the test passes whether or not
  the allocator reuses it.
- `test_git_integration`'s `cleanup()` checks every function: a window left waiting for an answer
  fails the function that left it.
- Through ctest: the suite alone passed with the fix, with the machine already at a load of about
  30 (a solve running in the installed Qucs-S, a fuzzer in another session). It then passed in five
  rounds of `ctest -j4` alongside `test_git_fuzz`, `test_mcp_server` and `test_running_jobs`, at load
  averages of 35-40, all four suites passing in every round. The first three rounds ran before
  `onWorktreeWindow`'s wait was raised to a minute, the last two on the code as committed.
- Not run: the ASan build, and the full suite (another session's full check would cover it).
