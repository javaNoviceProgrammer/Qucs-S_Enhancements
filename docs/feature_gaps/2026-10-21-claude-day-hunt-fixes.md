# The day-features hunt's findings, fixed

The user (2026-10-21): *"Fix all the findings, then commit and build the DMG"*, of
[the hunt of the day's features](../bug_hunts/2026-10-21-day-features.md): ten findings and seven
notes, from that morning's `da357b4` to `271f48c`. Each is fixed below, with its test, as are the
notes but N4 (not reproduced); one fix (N3) has no test.

## A. Wrong results taken without a word

**A1. Constants that are not a finite number.** `evaluateConstant` refuses NaN and infinity, saying
which: "it is inf, not a finite number". It used to return them, and its callers' checks (`x < 0`,
`!(x > 0)`) let one or the other through. This covers the 125 reads of a constant in the multiphysics
code at once. The spectrum's energies (broadening, fade, Urbach width) and the quantum node's
energies are compiled directly, so they check too. On the hunt's model, an infinite carrier density
that gave the well gain is now "Optical Spectrum: the carriers: it is inf, not a finite number". The
same for an infinite index, a NaN broadening and the rest. All 28 multiphysics suites pass with it.

**A2. Masetti's mobility.** Covered by A1: an infinite µ1 (which solved, a diode's current 170 times
too small), an infinite µmax, and a NaN m1 or Pc are refused. Their messages now name the model and
the constant: "domain 1: Masetti's mobility, mu1n: it is inf, not a finite number". It used to name the
domain alone. A µmax that is not a number is no longer said a second time as "must be more than 0".

## B. Bugs

**B1. The Claude panel's math, in quadratic time.** A render builds the transcript in one edit block,
laid out once at its end; it was laid out again at each insertion, from it to the end. And
`placeMath` finds a reply's marks in one pass instead of searching for each from the reply's start
(the Markdown viewer's fix of `ce9d212`). One reply of 2000 formulas: 34 s a render before, now
124 ms the first time (typesetting) and 49 ms after.

**B2. An empty image link.** `imageFile` keeps no name without a path of its own (`![x]()`, `#part`,
`?v=2`), and no folder (`.`, `..`). Only a file, or one not there yet, is watched. A file made beside
the document no longer renders it again.

**B3. Send to Background's refusal.** A request remembers whether its command was a task when it was
sent (`a_backgroundAskedOfTask`). Only a refusal of such a request is final. One refused before, the
task said meanwhile, is asked again.

**B4. `fem_build`'s timeout.** `fem_build` waits for a process flow's geometry up to its `timeout`, as
for a mesh. Past it, it says how far the flow is and that it goes on: "The geometry is still being
built after 1 s, 33 % - Building the geometry: Implant, step 2 of 3: it goes on (fem_build again
waits more; fem_stop stops it)". It used to wait without a limit.

**B5. A file dated in the future.** `QucsDoc::changedOnDisk(date)` replaces the four comparisons of a
file's date with the document's (the watcher, Save's overwrite check, Close's, the git tools'). A date
newer than the last load or save is a change, as before. When that date itself lies in the future (a
file unpacked from a zone ahead, a fast clock), any other date is a change too. Comparing "differs"
alone would not do: a model's and a sheet's saves record the present time, not the file's, and every
save would have read the file again.

**B6. A call sent to the background, held back.** `fem_solve`, `fem_build` and `fem_stop` are answered
when what they wait for ends: `MultiphysicsDoc::whenJobEnds` is told by the model's `stateChanged`
or a timer. They used to wait in a loop of their own (`waitForJob`), in which every later call ran,
nested. A call so waiting could not return until each later one had, so a `fem_solve` sent to the
background was answered only after them. `whenJobEnds` holds the model as a wait did: a tab closed
meanwhile is deleted once the call is answered. If the model is deleted meanwhile, the call is told.
The three run with the dispatcher's other asynchronous tools (`simulate`, `console`), and like them do
not count as their conversation's edits while they wait.

*After the release (CI of 38491ef):* `QucsControl::callNow`, which the tests, scripts and menu actions use,
gave up on an asynchronous tool after its own timeout. A sanitized Linux `fem_solve` outlasted a test's 120 s, and
its answer, at the job's end, wrote into `callNow`'s frame, gone: a stack-use-after-return. `callNow` now keeps
what the answer writes past its own return, and waits as long as the tool waits by itself
(`QucsControl::waitsFor`: `fem_solve`'s `timeout`, 600 s unless given; `fem_build`'s, 300; `fem_stop`'s, 30;
another's `timeout` given), as it did while these were answered within the call. Test:
`test_multiphysics_doc aToolsOwnWaitIsWaitedFor` (a solve called with 1 ms answered; the old wait fails it, and with
the old storage ASan reports the write).

The same CI's `test_claude_code aLongReplyIsReadInLinearTime` read 4 times the unclosed comments 8.05 and 7.9 times
as long, its code unchanged since the last green run. Here the reading is linear to 400,000 characters, 4.0 a step,
in Release and sanitized alike. Under the sanitizers each access touches their shadow too, and the larger text may
outgrow a cache: the comments searched to the end again (a quadratic reading) read 9.9 times as long sanitized here,
14.6 in Release, so no bound tells the two apart there. The test now times the two lengths in turn, judges the ratio
(under 8) in Release builds, CI's macOS among them, and only says it under the sanitizers.

**B7. Streaming over a math-heavy conversation.** The panel's typeset formulas are kept in a
`QCache` bounded by their images' size (64 MB). The old cache was cleared past 400 formulas, so a
longer conversation typeset most of its math again at each render, every 40 ms while a reply was
written. With B1, 50 streamed pieces over 1000 formulas take 1.1 s for 1 s (5.6 s before), the
longest stall 25 ms (311 ms).

## C. Time

**C1. Images decoded at each render.** The Markdown preview is a `QTextBrowser` of its own
(`PreviewBrowser`) whose `loadResource` keeps each decoded image while its file's size and dates stay
as read (a 512 MB `QCache`). The kept image shares its pixels with the document's while it is shown.
Eight 4000x3000 photos: 0.53 s a render before, 1 ms now. A key typed returns at once.

## Notes

- **N1.** Periodic ends on a 2D quantum model are said: "periodic ends are a line's (1D): the walls
  bound this one".
- **N2.** `excitonpairs` below 0 is said: "taken as 0, the defaults". One that is not a whole number
  the model already refused.
- **N3.** `TextDoc::load` takes the file's date before opening it, not after: a file renamed over it
  between the two is read again later rather than dated as the one read. *No test*: nothing can step
  in between the date and the opening.
- **N4.** Not reproduced; left.
- **N5.** The window's watcher and the preview's image watcher wait 400 ms after a change, but no
  longer than 1.5 s after the first (`misc::restartUpTo`): where each change is told at once (Linux's
  inotify), a file written all the time held them off for good.
- **N6.** Covered by B2.
- **N7.** A temperature that is not a finite number is said as such (A1), not as "its material has no
  band gap". One past 2000 K is warned of: "past the material library's fits (they hold to about
  1000 K)".

## Tests

| Finding | Test |
|---|---|
| A1, N2, N7 | `test_fem_quantum constantsNotFiniteRefused`: eight hostile spectrum constants, each refused and said; the quantum node's `drawn` `1/0` refused; `excitonpairs` -3 said, "lots" refused; T `1/0` said as not finite, 1e9 K warned of |
| A2 | `test_fem_mobility masettisConstantsFinite`: µ1 and µmax `1/0`, m1 and Pc `sqrt(-1)`: refused, the constant named, no "singular", not said twice |
| N1 | `test_fem_quantum periodicEndsOfALineAlone` |
| B1, B7 | `test_claude_code longRepliesMathRendersAtOnce`: 1000 formulas in one reply (a derivation, a paragraph each) rendered in fewer than 20 changes of the transcript; drawn again, none typeset again; four times the formulas in less than eight times the time |
| B3 | `test_claude_code aRefusalBeforeTheTaskIsAskedAgain`: a fake program says the task between the asking and the refusal: asked twice, sent, nothing said |
| B2 | `test_markdown_doc imagesWrittenAgainRenderAgain`, extended: `![x]()`, `#part`, `.` and `pictures/..` not watched; a file made beside the document renders nothing |
| C1 | `test_markdown_doc imagesDecodedOnce`: the shown image's `cacheKey` the same after two renders, another once its file changes |
| B5 | `test_markdown_doc aFileDatedInTheFutureIsReadAgain`: a file dated 3 h ahead, not read again unchanged, read again once written; `test_multiphysics_doc aModelSavedHereIsNotReadAgain`: a model saved here keeps its steps of Undo |
| N5 | `test_markdown_doc changesPutOffAtMostOneAndAHalfSeconds`: changes 100 ms apart for 3.5 s fire the timer between 1.5 and 2.4 s; one change after 400 ms |
| B4 | `test_multiphysics_doc aFlowsBuildWaitedForUpToItsTimeout`: a flow with a 600 min anneal, `fem_build` with `timeout` 1 answers within 3 s, its step said |
| B6 | `test_multiphysics_doc aWaitingCallIsNotHeldBackByLaterOnes`: a `fem_solve` of a quick flow, not answered within its call; a later `fem_build` of a slow one, waited for in its own loop; the solve answered half a second and more before the build |

**Breaks.** 22 breaks, 21 caught:
- A1: `evaluateConstant`'s check, the spectrum's energies' check, the quantum node's energies' check;
- A2: the constant's name, µmax said twice;
- N7: the warning past 2000 K, the temperature said twice;
- N1, N2: their warnings;
- B1: the single edit, the marks' one pass;
- B7: the cache cleared past 400 again;
- B3: the refusal judged when it arrives;
- B2: folders watched;
- C1: every image decoded again, a changed one not;
- B5: only a newer date a change, any other date a change;
- N5: put off without a limit;
- B4: the geometry waited for without a limit;
- B6: `whenJobEnds` a loop of its own again.

Not caught: N3 (dated after the opening), as said above.

Four were not caught at first:
- **The quantum node's energies' check:** no test set one. Its `drawn` is now tried.
- **The marks' one pass:** one reply is one paragraph, and a search within a paragraph is a fast
  `indexOf` (500 and 2000 formulas: 54 and 220 ms with it, a ratio of 4, within the bound). The test's
  reply is now a derivation, a paragraph each, which a search crosses block by block: 36 against
  500 ms broken (14 times), 15.6 against 61 ms fixed.
- **"Any other date a change":** no test saved a model. `aModelSavedHereIsNotReadAgain` now does.
  Read again by its own save, its steps of Undo were gone.
- **B2's check for a link of no path of its own:** it was redundant (such a link resolves to the
  folder, which the folder's check leaves out), and it is gone.

The whole suite passes in Release (135). The new tests and those near them pass under ASan with no
report; the N2 case solves no excitons, so the test takes 9 s there (it ran past CI's 300 s with
coupled ones). The CI lints pass.
