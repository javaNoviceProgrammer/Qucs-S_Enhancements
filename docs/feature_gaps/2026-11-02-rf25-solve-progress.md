# A progress bar for the RF 2.5D solve

The user's question of 2 November: "by the way, why RF 2.5D doesn't have a progress bar for when it's solving?"

## What there was

Progress was text only. The status line ended "; solving 3 of 11 frequencies" for a full-wave solve, "; calibrating
the ports" while the ports' feeds were calibrated, and "; solving" for the quasi-static extraction. The notes and the
panel's Results node said "Solving full-wave: 3 of 11 frequencies…". The count is at the end of the status line, so
it is the first thing cut when the line gives way (`ElidedLabel`, 6b2e3d7). The calibration said no count at all,
though on the tests' microstrip it takes twice as long as the sweep: 83 ms for the sweep of three frequencies here
in Release, 255 with the strips of one port's feed.

## What changed

**A bar at the end of the status line** (`rfProgress`, `RF25Doc::progressBar()`). It is shown once a solve has
taken a quarter of a second, the multiphysics tab's rule, so a coarse mesh solved in milliseconds never flashes it.
It is hidden when no solve is going on: at the end of a solve, at a Stop, and when a solve is refused. What it shows:

| While | The bar | Its tool tip |
|---|---|---|
| A solve waits for the shapes and the mesh of an edit | Busy | "Solving once the shapes and the mesh of the last edit are made…" |
| The quasi-static extraction | Busy: one dense solve, with nothing in it to count | "Solving…" |
| The full-wave sweep | Frequencies solved of those to solve, the status line's own count | "Solving full-wave: 3 of 11 frequencies…" |
| The ports' calibration | Busy while the first strip is meshed and set up, then the strips' frequencies solved of all they will solve | "Taking out the ports' own discontinuities: 5 of 22 frequencies of the strips of their feeds solved" |

The status line, the notes and the panel say what they said: the tests and the `rf25_*` tools read them. The
Solve/Stop button works as before.

**The calibration's count.** `calibrate()` (rf25_deembed.h) solves two strips of each feed. Each strip is solved at
the sweep's frequencies and, where the sweep starts too high for the shorter strip's phase to be followed up from,
at one frequency below the sweep too. The tab cannot know which without taking `calibrate()`'s choice of strip
lengths for its own, so the solver tells it. `FullWaveControl::begins` is called as a solve begins its frequencies,
after its setup, with how many there are. The tab counts the feeds to calibrate before the first one begins: one
calibration per feed, however many ports share it. Its total is the frequencies of the strips begun, plus, for each
strip not yet begun, as many as the strips begun had on average. For one feed that is exact from its first strip,
since both of a feed's strips have the same frequencies. For several feeds it is exact once the last has begun,
and before that it is out by at most one frequency a feed. `begins` is called after the size checks, so a strip
found too large, which `calibrate()` cuts shorter and tries again, counts nothing.

**Its size.** The bar is 160 pixels wide, fixed, as the multiphysics tab's is. The status line gives way to it, and
the tab's least width stays its bar of buttons': 827 pixels offscreen while solving (831 while not, since the
button's "Stop" is narrower than its "Solve"), against some 220 for the status line with the bar. It is also capped
at the line's height. Fusion's progress bar asks for 23 pixels and macOS's for 20, where the line is 19. Uncapped,
the line would grow and the view above it shrink by a few pixels each time a solve started.

**A solve started again keeps its bar.** When an edit's mesh overtakes a solve that is going on, the solve is
started again on the new mesh (`prepareDone()`). That went through `stopSolving()`, which now hides the bar and
starts its quarter of a second again, so the bar would go and come back a moment later. `letSolveGo()` is
`stopSolving()` without the bar. It is used in the two places where a solve is let go only to be started again at
once: there, and at the start of `solveWith()`.

**The timer.** The 400 ms timer used to run for full-wave solves only, to refresh the notes and the panel. It now
runs while any solve goes on, keeping the bar up to date, and still refreshes the notes and the panel for a
full-wave solve only.

## A hook for the test

`holdSolves()` holds a solve before it computes anything. Seeing a sweep part way needs more than that: the test
line's three frequencies are solved side by side, three threads at once, in 83 ms here in Release, long before the
bar's timer first ticks at 400 ms. `FullWaveControl::allowed` is a count of the frequencies that may begin. Each
frequency takes one, the next waits while there are none, and a stop ends the wait. `RF25Doc::allowFrequencies(n)`
sets it for the tab's solves, the sweep's frequencies first and then the strips'. It is a test's knob in the solver,
as `mostUnknowns` and `mostBytes` already are. A frequency waiting on it is the one its thread took, so the count of
frequencies solved is exact whatever the number of threads.

## Tested

`aSolveShowsHowFarItHasGot` (`test_rf25_doc`), on the tests' full-wave microstrip of three frequencies:

- Opened, the bar is hidden. A quasi-static solve, held, does not show the bar at once, then shows it busy. The
  status line still ends "; solving". The bar sits at the end of the status line. The tab's least width is what it
  was before the bar showed, the window asks no more width and is as wide as it was, and the splitter above is as
  tall as it was. Released, the solve ends and the bar is hidden.
- The full-wave solve, by the Solve button, with no frequency allowed: 0 of 3. With one allowed: 1 of 3 once it is
  solved, the status line "; solving 1 of 3 frequencies" and the tool tip "1 of 3". With the rest allowed it ends,
  the bar is hidden and the button says Solve.
- Stopped by the button while held: the bar is hidden at once and stays hidden.
- Held, then overtaken by an edit's mesh: the solve is started again and the bar is still shown.
- Port 1 de-embedded 20 µm along its feed, the sweep's three frequencies allowed: once the status line says ";
  calibrating the ports" and the first strip has begun, the bar counts 0 of 6 (or 8 with a frequency below the
  sweep). With one more allowed: 1 of 6, and the tool tip says so. Then exactly the bar's remaining count is allowed,
  and the solve ends. So the bar counts all that the strips solve, no fewer, and the line is de-embedded by two
  strips of its feed.

The function takes about 4 seconds in Release, three runs alike, and passes under the macOS style as well as
Fusion. Under the sanitizers it takes about 13 seconds.

## Run

The full Release ctest passes, 151 of 151, four at a time as CI runs them. Under the sanitizers (`build-asan`)
`test_rf25_doc` takes 158 s, `test_long_lines` 73 s, `test_rf25_fullwave` 483 s and `test_rf25_deembed` 244 s, all
passing; the solver's two suites were run because `FullWaveControl` and the sweep's loop changed. The lints
`check-tool-arguments.py`, `check-json-nesting.py`, `check-third-party.py` and `check-shaders.py` pass. Nothing used
here is newer than Qt 6.10.3, CI's version: `QProgressBar`, `QElapsedTimer` and `QTest::qWaitFor(predicate, int)`
are all older than Qt 6.

## Broken on purpose

`docs/feature_gaps/2026-11-02-rf25-solve-progress/breaks.py` holds twenty-three:

- **Shown and hidden:** never shown; shown at once; left shown when the solve ends; left shown by a Stop until a
  tick of its timer; its timer not started, as it was not for a quasi-static solve; its timer's tick leaving it as
  it was.
- **What it counts:** the quasi-static extraction counted out of one, not busy; the sweep busy; the sweep counted out
  of one frequency more than it has; the sweep's count standing still; its tool tip saying nothing of it; the
  calibration busy throughout; the calibration counted out of the strips begun alone; the feeds to calibrate not
  counted; the strips' frequencies solved not counted.
- **What the solver tells it:** `begins` never called; the allowance ignored.
- **A solve started again:** `prepareDone()` and `solveWith()` letting the solve go through `stopSolving()`.
- **Its place and size:** as tall as it would be; as wide as it likes (a least width of 1600 pixels); before the
  status line, not at its end.
- **The status line:** its count said otherwise.

All twenty-three were caught, each by `aSolveShowsHowFarItHasGot` at its own check: twenty within 17 seconds. The
other three leave the calibration's count waiting for something that never comes: busy throughout, `begins` never
called, and the strips' solved frequencies not counted. They fail at the test's two-minute bounds on the solver.

Those bounds were `QTRY_*` checks at first, and the three were caught by QtTest's five-minute limit on a function,
not by the test. A `QTRY_*` check that fails waits twice its timeout again, to say whether it would have passed
later, so a two-minute check fails after six. The waits for the solver are `QTest::qWaitFor` now, which ends at its
timeout. The waits for the bar's 400 ms timer are still `QTRY_*` at five seconds.

## Not done

- **The panel's Results node** says "Solving full-wave: 3 of 3 frequencies…" while the ports are calibrated: it reads
  `frequenciesSolved()` and `frequenciesToSolve()` alone. It was left as it was, with the other texts.
- **The quasi-static extraction is busy, not counted.** It is one dense solve and one LU, which report nothing as
  they go.
- **The button says Solve during a second making of the shapes and the mesh** when an edit came while the first was
  being made under a solve that waited for it (`prepareDone()`'s `a_prepareAgain` branch): `letSolveGo()` names the
  button Solve, and nothing names it Stop again until the solve starts. It was so before; now the bar shows busy
  meanwhile, as `isSolving()` says.
- **The `rf25_*` tools** report no progress of their own. They read the status line, which says what it said.
