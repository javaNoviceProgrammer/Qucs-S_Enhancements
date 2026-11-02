# A progress bar for the RF 2.5D solve

The user's question of 2 November: "by the way, why RF 2.5D doesn't have a progress bar for when it's solving?"

## What there was

Progress was text only. The status line ended "; solving 3 of 11 frequencies" for a full-wave solve, "; calibrating
the ports" while the ports' feeds were calibrated, and "; solving" for the quasi-static extraction. The notes and the
panel's Results node said "Solving full-wave: 3 of 11 frequencies…". The count is at the end of the status line, so
it is the first thing cut when the line gives way (`ElidedLabel`, 6b2e3d7). The calibration said no count at all,
though on the tests' microstrip it takes three times as long as the sweep: 81 ms for the sweep of three frequencies
here in Release, 335 with the three strips of one port's feed.

## What changed

**A bar at the end of the status line** (`rfProgress`, `RF25Doc::progressBar()`). It is shown once a solve has
taken a quarter of a second, the multiphysics tab's rule, so a coarse mesh solved in milliseconds never flashes it.
It is hidden when no solve is going on: at the end of a solve, at a Stop, and when a solve is refused. What it shows:

| While | The bar | Its tool tip |
|---|---|---|
| A solve waits for the shapes and the mesh of an edit | Busy | "Solving once the shapes and the mesh of the last edit are made…" |
| The quasi-static extraction | Busy: one dense solve, with nothing in it to count | "Solving…" |
| The full-wave sweep | Frequencies solved of those to solve, the status line's own count | "Solving full-wave: 3 of 11 frequencies…" |
| The ports' calibration | Busy while its first strip is meshed and set up, then its first length's strips' frequencies solved of all they will solve; busy while shorter strips are solved | "Taking out the ports' own discontinuities: 5 of 33 frequencies of the strips of their feeds solved", and " (feed 1 of 2)" after it where there are several |

The status line, the notes and the panel say what they said: the tests and the `rf25_*` tools read them. The
Solve/Stop button works as before.

**The calibration's count.** `calibrate()` (rf25_deembed.h) solves strips of each feed. Since main's ab18dfe it
solves three of one length (`kStripsPerLength`: the shorter, the longer and a third that checks the two). Each is
solved at the sweep's frequencies and, where the sweep starts too high for the shorter strip's phase to be followed
up from, at one frequency below the sweep too. Where the three do not agree on one line, it solves three of a
shorter length beside them, up to three times. How many of those there will be, nothing can say before they are
solved.

So the bar counts a calibration's strips of its first length. The tab cannot know whether there is a frequency
below the sweep without taking `calibrate()`'s choice of lengths for its own, so the solver tells it:
`FullWaveControl::begins` is called as a solve begins its frequencies, after its setup, with how many there are. The
first strip's count is the count of all three. The bar is busy until that strip begins, and busy again while
shorter strips are solved. Several calibrations (feeds unlike, or strips asked of different lengths) are counted
one after another, each from nought, and the bar's tool tip says which ("feed 1 of 2"). `kStripsPerLength` is the
constant `solveStrips()` sizes its strips by, so the tab's count cannot drift from what it solves. `begins` is
called after the size checks, so a strip found too large to solve, which `calibrate()` cuts shorter and tries
again, counts nothing.

This branch was cut before ab18dfe, and its first count was of two strips a feed. Merged with it, the count became
the one above, per calibration, keyed as the tab keys its calibrations now (the feed, and the strips' length asked).

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
line's three frequencies are solved side by side, three threads at once, in 81 ms here in Release, long before the
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
- Both ports de-embedded 20 µm along their feeds, by strips of 300 and 400 µm: two calibrations, and a length asked
  solves no shorter strips, so the count is exact. With the sweep's three frequencies allowed, once the status line
  says "; calibrating the ports" and the first strip has begun, the bar counts 0 of 9 (three strips at three
  frequencies) and its tool tip says "feed 1 of 2". With one more allowed: 1 of 9, and the tool tip says so. With
  the other eight allowed, the second calibration begins: 0 of 9 again, "feed 2 of 2". With exactly nine more
  allowed, the solve ends, so the bar counts all that the strips solve. Both ports come out de-embedded by three
  strips of their feeds.

Run alone with the window's setup, the function takes about 5 seconds in Release, three runs alike, and passes
under the macOS style as well as Fusion. Under the sanitizers it takes about 18 seconds.

## Run

On the branch merged with ab18dfe, the full Release ctest passes, 151 of 151, four at a time as CI runs them. Under
the sanitizers (`build-asan`) `test_rf25_doc` takes 179 s, `test_long_lines` 71 s, `test_rf25_fullwave` 495 s and
`test_rf25_deembed` 382 s, all passing, each beside the breaks' builds; the solver's two suites were run because
`FullWaveControl`, the sweep's loop and `solveStrips()` changed. The lints `check-tool-arguments.py`,
`check-json-nesting.py`, `check-third-party.py` and `check-shaders.py` pass. Nothing used here is newer than Qt
6.10.3, CI's version: `QProgressBar`, `QElapsedTimer` and `QTest::qWaitFor(predicate, int)` are all older than Qt 6.

## Broken on purpose

`docs/feature_gaps/2026-11-02-rf25-solve-progress/breaks.py` holds twenty-five:

- **Shown and hidden:** never shown; shown at once; left shown when the solve ends; left shown by a Stop until a
  tick of its timer; its timer not started, as it was not for a quasi-static solve; its timer's tick leaving it as
  it was.
- **What it counts:** the quasi-static extraction counted out of one, not busy; the sweep busy; the sweep counted out
  of one frequency more than it has; the sweep's count standing still; its tool tip saying nothing of it; the
  calibration busy throughout; a calibration counted out of the strips begun alone; the calibrations to make not
  counted; the calibrations begun not counted; each calibration not counted from nought; the strips' frequencies
  solved not counted.
- **What the solver tells it:** `begins` never called; the allowance ignored.
- **A solve started again:** `prepareDone()` and `solveWith()` letting the solve go through `stopSolving()`.
- **Its place and size:** as tall as it would be; as wide as it likes (a least width of 1600 pixels); before the
  status line, not at its end.
- **The status line:** its count said otherwise.

All twenty-five were caught, each by `aSolveShowsHowFarItHasGot` at its own check: twenty-one within 17 seconds.
The other four leave the calibration's count waiting for something that never comes: busy throughout, `begins`
never called, the strips' solved frequencies not counted, and the second calibration not counted from nought. They
fail at the test's two-minute bounds on the solver.

Those bounds were `QTRY_*` checks at first, and such breaks were caught by QtTest's five-minute limit on a function,
not by the test. A `QTRY_*` check that fails waits twice its timeout again, to say whether it would have passed
later, so a two-minute check fails after six. The waits for the solver are `QTest::qWaitFor` now, which ends at its
timeout. The waits for the bar's 400 ms timer are still `QTRY_*` at five seconds.

## Not done

- **The bar going busy while shorter strips are solved is not tested.** A calibration whose strips of the first
  length disagree is a solve of minutes (`test_rf25_deembed`'s MOM feed), and whether they agree is the solver's
  numerics, which the phase E round is still changing. The test asks for the strips' lengths, which solves no
  shorter ones, so that its count is exact.
- **The de-embedding's line of each port** still says "taken out by two strips of its feed, 900 and 1800 µm long",
  where ab18dfe's calibration solves three and its own line says so ("Three strips of the feed, … 900, 1800 and 1456
  µm long"). That text is `deembedded()`'s, which the phase E round owns and the tools read, so it was left.
- **The panel's Results node** says "Solving full-wave: 3 of 3 frequencies…" while the ports are calibrated: it reads
  `frequenciesSolved()` and `frequenciesToSolve()` alone. It was left as it was, with the other texts.
- **The quasi-static extraction is busy, not counted.** It is one dense solve and one LU, which report nothing as
  they go.
- **The button says Solve during a second making of the shapes and the mesh** when an edit came while the first was
  being made under a solve that waited for it (`prepareDone()`'s `a_prepareAgain` branch): `letSolveGo()` names the
  button Solve, and nothing names it Stop again until the solve starts. It was so before; now the bar shows busy
  meanwhile, as `isSolving()` says.
- **The `rf25_*` tools** report no progress of their own. They read the status line, which says what it said.
