# Bug hunt: the day's features

*21 October 2026 - Qucs-S 27.0.2, `271f48c` (the Release build; the ASan/UBSan build where said).
One hour, 18:49 to 19:49, in the foreground: hunting until 19:40 (probes through the tools and in
throwaway tests, four fuzz runs, reading), the write-up after. The findings are written up as they
came; none is fixed here. All fixed, the notes too but N4, in the commit that adds
[docs/feature_gaps/2026-10-21-claude-day-hunt-fixes.md](../feature_gaps/2026-10-21-claude-day-hunt-fixes.md).*

The user's request: "continue hunting for bugs in our enhanced qucs for one hour and document
your findings ... If you find a bug don't stop to fix it". The morning's hunt
([2026-10-21-fuzz.md](2026-10-21-fuzz.md)) fuzzed everything up to `8c36b43`. This one looks at what
came after:
- the process flow off the GUI thread (`da357b4`);
- the math typeset in the user's prompts (`2258e84`);
- the mobility update check (`8d7cc70`);
- the Miller round: the spectrum's new options, periodic ends, the 8x8's spurious filter (`68e5f16`);
- the Markdown viewer's image watch and single-layout render (`ce9d212`);
- Send to Background (`271f48c`).

Each run had its own HOME, settings and cache in a scratch folder. The probe scripts are in
[2026-10-21-day-features/](2026-10-21-day-features/).

## Summary

| | Finding | Since |
|---|---|---|
| **A1** | The spectrum's constants take NaN and infinity: an infinite carrier density gives gain, an infinite index no absorption, a NaN broadening none - no error; 110 of 125 constant reads unchecked for finiteness | before (`linewidth`, `density`, `nr`), `68e5f16` (`urbach`) |
| **A2** | Masetti's mobility: an infinite µ1 solves with a forward current 170 times too small; an infinite µmax or a NaN m1 or Pc fails as "a singular Jacobian", no value named | before; µmax since `8d7cc70` |
| **B1** | The Claude panel's math in a reply is rendered in quadratic time (1000 formulas 9.5 s, 2000 34 s), again at each event | before; prompts since `2258e84` |
| **B2** | An empty image link (`![x]()`) watches the Markdown's own folder: every file made beside it renders the document again | `ce9d212` |
| **B3** | Send to Background takes a "not a task yet" refusal for final when the task was announced just before it | `271f48c` |
| **B4** | `fem_build` waits for a process flow's geometry without a limit (ASan: past 120 s with `timeout: 1`), no progress said | `da357b4` |
| **B5** | A document whose file is dated in the future (an unzipped file, a fast clock) misses every later change by another program until the clock passes that date | before |
| **B6** | A Qucs-S call sent to the background still waits in a nested event loop: its result is held back until every later call (a long solve, a dialog) has returned | `271f48c` |
| **B7** | While a reply streams, the whole conversation's math is typeset again every 40 ms: 1000 formulas over 40 replies make 1 s of streaming take 5.6 s, stalls of 311 ms | before |
| **C1** | The Markdown preview decodes every image again at each render: 8 photos of 4000x3000 freeze the window 0.53 s after each pause in typing | before; more often since `ce9d212` |

## A. Wrong results taken without a word

### A1. The spectrum's constants take NaN and infinity

`evaluateConstant` (`fem_expr.cpp`) returns what the expression gives, NaN and infinity included, and
leaves the check to its caller. The spectrum's checks (`fem_solver.cpp` 3330-3400) are written as
`x < 0` (NaN passes) or `!(x > 0)` (infinity passes):
- the broadening, the fade and the Urbach width (`energy()`: `constant() < 0`);
- the carriers' density (`*density < 0`);
- the index and the excitons' permittivity (`!(*n > 0)`).

Through the tools, on the shipped `gaas_quantum_well.qfem`, the Absorption study solves each with no
error and no warning of its own. α_TE at 1.85 eV (6.6 meV broadening: 2.223e6 1/m):

| Option | Value | α_TE at 1.85 eV |
|---|---|---|
| `density` | `1/0` | **-2.223e6 1/m: gain**, only "the electrons' quasi-Fermi level is near the highest state" said |
| `nr` | `1/0` | **0** at every energy |
| `linewidth` | `sqrt(-1)` | 1.946e6, the same as `0`: no broadening, silently |
| `linewidth` | `1/0` | 1.738e6 |
| `density`, `fade` | `sqrt(-1)` | 2.223e6, as if not given |
| `epsr`, `urbach` (bulk excitons) | `1/0` | 2.223e6 |

`linewidthslope` alone is checked both ways (`*d >= 0 && *d <= 10`). Across `multiphysics/`, 110 of
the 125 calls of `evaluateConstant` have no finiteness check within five lines. Many have a range check
that keeps NaN out, but infinity through.

*Proposed fix:* `evaluateConstant` refuses a value that is not finite (an error naming it), as
`bandParameters` already does for an alloy's fraction ("x = inf is not between 0 and 1").

*Probe:* `opts.py` (`QUCS=<app> HUNT_RUNS=<scratch> python3 opts.py [case ...]`).

### A2. Masetti's mobility: an infinite µ1 solves, 170 times off; a NaN is "a singular Jacobian"

The same pattern in the semiconductor's Masetti model (`fem_semiconductor.cpp` ~1490, its µmax since
`8d7cc70`). µmax is checked with `!(x > 0)`, which lets infinity through; m1, m2, µ1 and Pc are not
checked at all (only Cr and Cs, and the exponents' finiteness). On the shipped `pn_diode.qfem` at
0.7 V, with a Masetti model over both sides (I_a 24.34 µA as given):

| Changed | Result |
|---|---|
| `mu1n` = `1/0` | **solves: I_a = 144 nA**, 170 times less, no word |
| `mmaxn` = `1/0` | fails: "no solution found on the way to the contacts' voltages ... its Jacobian is singular" |
| `m1n` = `sqrt(-1)` | the same singular Jacobian |
| `pcn` = `sqrt(-1)` | the same singular Jacobian |
| `mmaxn` = -1 | refused: "Masetti's µmax must be more than 0" (right) |

A NaN or an infinite constant shows as a numerical failure, naming no value: one looks at the mesh and
the contacts. One case gives a wrong current.

*Proposed fix:* A1's, which covers it; or check each of the eight constants finite here, as the Arora
model next to it does (`finite` over `m.ar`).

*Probe:* `masetti.py`.

## B. Bugs

### B1. The Claude panel renders a reply's math in time quadratic in its formulas

`ClaudeCodePanel::placeMath` (`claudecodepanel.cpp`, shared by replies and, since `2258e84`, the
user's prompts) is the loop `qucs_s::markdown::render` had until `ce9d212`. For each formula it searches
the document for its mark from the reply's start, and inserts the typeset formula, which lays the rest
of the document out again. The transcript is rendered whole again at each new entry (a note, a tool, a
reply). One assistant reply of n inline formulas, `renderNow()` offscreen:

| Formulas in the reply | A render | Again | After a note |
|---|---|---|---|
| 250 | 0.74 s | 0.71 s | 0.71 s |
| 500 | 2.8 s | 2.8 s | 2.7 s |
| 1000 | 9.5 s | 9.6 s | 9.7 s |
| 2000 | 34 s | 34 s | 34 s |

Spread over many replies, it stays near linear: 40 replies of 25 formulas take 0.30 s, 10 of 25 take
0.10 s. A long derivation in one reply is enough: at 250 formulas, every later event of the
conversation freezes the window for 0.7 s.

*Proposed fix:* as `ce9d212` did for the Markdown viewer. Find the marks in one pass, insert them in
one edit block, and keep each reply's typeset document rather than typesetting it again at every
render.

*Probe:* `huntMathReply` (throwaway, in `test_claude_code`: a reply fed with `handleLine`).

### B2. An empty image link watches the Markdown's own folder (since `ce9d212`)

`imageFile` (`markdowndoc.cpp`) resolves an image's name against the document's base URL. An empty name
(`![a figure to come]()`, common in a draft) or `.` resolves to the folder itself, which exists.
`watchImages` then keeps the folder as an image, with its dates as the stamp. A folder's modification
date changes whenever a file is made or removed in it, so every file made beside the document renders
it again. In the probe, five CSV files made in the folder 0.7 s apart caused 12 content changes of the
preview (each render makes two to six). A long document in a folder a script writes into is rendered
again and again, each render 0.4 s for 3000 lines.

*Proposed fix:* keep only regular files (`QFileInfo::isFile()`, or not existing yet) whose name is not
empty.

*Probe:* `huntEmptyImage` (throwaway, in `test_markdown_doc`).

### B3. Send to Background takes a refusal for final when the task came before it (since `271f48c`)

A command asked before it is a task is refused (`backgrounded: false`); the dock asks again when
`task_started` says it is one. `handleControlResponse` decides by whether the task is known *when the
refusal arrives*, not when the request was sent. When the program announces the task between taking the
request and answering it, the dock says "Bash could not be sent to the background." The panel has just
said "Sent to the background...", and the dock does not ask again. With a fake program sending
`task_started` just before the refusal: one request, that notice, the turn still waiting.

Whether Claude Code 2.1.285 can write the two in that order was not seen; its handler answers
asynchronously (`Promise.resolve().then(...)`), and the task's registration is asynchronous too.

*Proposed fix:* record, with the request, whether its task was known when it was sent
(`a_backgroundAsked` holds the tool use; add the flag). Treat as final only a refusal of a request
made after the task was known.

*Probe:* `huntBackgroundRace` (throwaway, in `test_claude_code`).

### B4. `fem_build` waits for a process flow's geometry without a limit

`fem_build`'s `timeout` is "mesh: seconds it is waited for". A process flow's geometry, built in the
background since `da357b4`, is "waited for" with no limit and no progress said. `fem_solve` instead
returns how far it got after its timeout. A flow of a substrate, an implant and a 120 min diffusion at
1100 °C (`flowtime.py`) takes 18.3 s to build in Release; on the ASan build, `fem_build` with
`timeout: 1` had not returned after 120 s, three times running (`flowrace.py`). A flow whose anneals take
minutes holds Claude's tool call, and the conversation, for minutes. Claude cannot ask how far it is
or decide to wait longer, though `fem_stop` would stop it.

*Proposed fix:* the timeout applies to the geometry too, with the flow's step and time step said
("Diffuse (Anneal), step 3 of 3, t = 41 min") as `fem_solve`'s progress is; a call past it returns that.

### B5. A document whose file is dated in the future misses every later change on disk

`QucsApp::documentsChangedOnDisk` (`qucs.cpp`) loads a changed file again only when
`lastModified() > getLastSaved()`. `TextDoc::load` sets that to the file's own date, on purpose, so that a
future-dated file is not reloaded for ever ("one dated in the future was newer than the load for good").
The other side of that choice: once the date is in the future, every change made by another program
carries the present date, earlier than it, and is skipped until the clock passes the file's date. A
`.md` dated 3 h ahead, opened, then written again (dated now): not refreshed (`huntFutureDate`, still
not after 6 s). Schematics and the other watched documents compare the same way.

Such dates are common enough:
- a ZIP stores local times without a zone, so one made in a zone ahead of ours unpacks hours in the
  future;
- copies keep a date from a machine with a fast clock;
- `touch -d`.

Claude's own Write and Edit are not affected: `reloadChangedFiles` loads without comparing dates. But
another program's (a script, an editor) is, and only for the window's watcher. This may be part of
what the user saw as "no auto-refresh".

*Proposed fix:* remember the file's date *and* size (or a hash of its start) at the load, and load again
when either differs from what is on disk, rather than when the date is newer.

### B6. A Qucs-S call sent to the background can be held back by any later call (since `271f48c`)

Send to Background answers a Qucs-S tool's call at once, but the host's tool goes on waiting as before.
`MultiphysicsDoc::waitForJob` (`multiphysicsdoc.cpp` 1447) waits in a nested `QEventLoop`, up to the
call's timeout (`fem_solve`: 600 s by default). Claude goes on, and its next Qucs-S call runs *inside*
that loop. Nested loops end in the reverse order of their start: when the first job ends, its loop is
told to quit, but it cannot return until every call begun after it has returned. A `fem_solve` sent to
the background, then a ten-minute solve of another model: the first's "has finished" message comes ten
minutes late, though its job ended long before. A modal dialog opened meanwhile holds it the same way.
Found by reading; not run.

*Proposed fix:* a call sent to the background stops waiting. The host is told (a "detach" of the call)
and returns from its loop at once; the dock delivers the result from the model's own `stateChanged`
when the job ends, not from the call's return.

### B7. While a reply streams, every formula of the conversation is typeset again every 40 ms

`onReplyStreamed` schedules a render at each piece of the reply; the render timer runs every 40 ms
(`a_renderTimer`). Each render builds the whole transcript again, typesetting every formula of every
earlier reply (B1's loop, and no cache of the typeset replies). A conversation of 40 replies of 25
formulas (0.30 s a render): 50 pieces of a new reply, sent 20 ms apart, took 5.6 s instead of 1 s,
the event loop stalled up to 311 ms at a time (`huntStreaming`). The panel and the window lag while
Claude writes in any math-heavy conversation, more as it grows.

*Proposed fix:* render the streaming reply alone (its own block at the end, replaced as it grows), and
keep the earlier replies' rendered documents (B1's fix) so that a render costs the new text only.

## C. Time

### C1. The Markdown preview decodes every image again at each render

`render` begins with `setMarkdown`, whose `clear()` empties the document's resources: the images it
read, and the decoded copies `SmoothImages` adds. Every render (250 ms after each pause in typing, in
Edit and Split modes too) reads and decodes each image file again. A document of eight 4000x3000 PNG
photos, in Split mode: 578, 545 and 531 ms a render. A key typed gave the event loop back after
830 ms, against 300 waited: the render took the rest (`huntBigImages`).

Since `ce9d212` the same happens whenever an image changes on disk; with B2, whenever any file in the
folder does.

*Proposed fix:* keep the decoded images across renders, keyed by file and stamp (`ImageStamp`
already has the key), and put them back into the new document's resources before it is laid out.
`clear()` then costs nothing but the text.

## Notes

- **N1.** `ends: periodic` on a 2D quantum model solves with no word: it applies to a 1D line alone
  (`a_periodic = a_1d && ...`), and the property is hidden in the dialog for 2D, but a file or a tool
  can set it (`quantum_wire.qfem` with it: "ok", no warning).
- **N2.** `excitonpairs` below 0, or not a number, is taken as 0, the defaults (12 or 16 pairs), with no
  word. Only above 64 is it said (`countsCapped`).
- **N3.** `TextDoc::load` (since `ce9d212`) takes the date after opening the file and before reading it.
  A file renamed over it in between is read as the old one (the open handle) but dated as the new one,
  and the new one is not loaded. The window is microseconds; the full fix is B5's.
- **N4.** One of the 30 quantum mutants (`strained_laser_gain.qfem` with a rectangle's position
  `"x"`) closed after 42.6 s on ASan, following a 20.4 s solve still running. Not reproduced in Release
  on the shipped models (each stops within 0.5 s); the mutant was not run again.
- **N5.** Linux's inotify was not checked for the Markdown watchers' debounce (see "Checked and found
  right").
- **N6.** An empty image link has variants: `![x](#part)` and `![x](?v=2)` resolve to the folder too
  (B2), by the same `QUrl` resolution against the base URL. Not probed separately.
- **N7.** The quantum node's temperature: NaN, 0 and -5 K are refused with the reason. `1/0` fails as
  "its material has no band gap and valence band offset ... give its compound": the library's gap,
  evaluated at an infinite T, is NaN. 1e9 K solves without a word, though the library's fits hold to
  about 1000 K (`temp.py`).

## Checked and found right

- **An alloy's fraction out of range.** AlGaAs barriers with `x` of 1.7, -0.3, `sqrt(-1)`, `1/0` and
  1e300 are refused, the value named: "AlGaAs: x = nan is not between 0 and 1" (`alloy.py`).
- **Process flows built in the background** (`da357b4`), on the ASan build:
  - 40 flows of the morning's kinds fuzzer (`fuzz_kinds.py process 2110 40`, plausible and hostile):
    no crash, hang or sanitizer report; 17 refused with clear reasons;
  - six flows with a 120 min diffusion, stopped and closed while their opening's build ran
    (`flowrace.py`): no crash.

  In Release, `fem_stop` took 1.4 s and a close 1.0 s, against the whole build's 18.3 s.
- **The quantum examples after the Miller round** (`68e5f16`): 30 mutants of the six quantum examples
  through open, build, solve, describe, evaluate and close on ASan (`fuzz_qfem.py` restricted to them,
  seed 2111): no crash or sanitizer report. The Miller model's 8x8 spectrum stopped at once at 37 %
  (`fem_stop` 0.0 s, "cancelled"), and closed at once mid-solve.
- **The user's prompts with math** (`2258e84`): "Compare <b>x</b> &amp; y < z with $\alpha$",
  "Price $5 and $10, with $\beta$", a Windows path with `$` in it, and an HTML comment beside display
  math are all shown as typed, with exactly the five formulas meant typeset (`huntPromptMath`).
- **The window** (`test_gui_monkey`, seed 21102, 2500 steps, ASan): passed, no sanitizer report.
- **The Miller model's States study**: 4-band, its levels in 0.4 s. Under the 8x8 (`kpLevels`) with 8,
  1 and 40 states: e1 0.65566 eV, hh1 -0.80869 eV, lh1 -0.82255 eV each time, 40 states in 13 s, no
  warning (`states8.py`, `states8b.py`).
- **Many images fitted to the view.** 100, 200 and 400 images wider than the view render in 19, 35
  and 86 ms. `fitImages` makes one change each, outside the render's single edit, but one file is
  decoded once a render (`huntManyImages`).
- **The Markdown renderer on ASan** (`test_markdown_doc fuzz`, seed 2112, 240 s): 5,682 documents,
  17,046 formulas, no report. This was today's single-edit render and its one pass over the marks.
- **The tools** (`fuzz_tools.py` on ASan): seed 2113, 600 calls, no event. Seed 2114 was cut by the
  hunt's 330 s limit at call 1610, with no crash, hang or sanitizer report up to there.
- **A symlinked image** (a report's `plot.png` linking to a results folder): its target written again in
  place is shown after 0.98 s, renamed over after 0.56 s (`huntSymlinkImage`, in
  `hunt-probes-symlink.patch`).
- **The Markdown watchers under constant writing.** The `.md` was appended to every 150 ms for 6 s, and
  a log beside a changed image was written every 150 ms. On macOS the watcher's events come about
  0.5 s apart, coalesced, so the 400 ms timers still fire: the image was shown again at 0.93 s, the text
  at 0.76 s. *Not checked:* Linux's inotify sends every event, so a timer restarted at each could
  wait until the writing stops.

## Reproducing

The scripts beside this report take the app's binary and a scratch folder:

```
QUCS=build/qucs/qucs-s.app/Contents/MacOS/qucs-s HUNT_RUNS=<scratch> HOME=<scratch>/home QUCS_CACHE_DIR=<scratch>/cache python3 docs/bug_hunts/2026-10-21-day-features/opts.py
```

- `opts.py` (A1), `masetti.py` (A2), `alloy.py`, `temp.py` (N7), `states8.py`, `states8b.py` and
  `stopmiller.py` (the Miller model, from the Miller round's scratch folder: give its `.qfem`) run models
  through the tools by `mq.py`, which borrows `fuzz_qfem.py`'s server from the hunt of 16 October.
- `flowrace.py` and `flowtime.py` (B4) borrow `fuzz_kinds.py`'s flow generator from the morning's hunt,
  with `KINDS_JSON` from `test_fem_engine dumpKinds` (`QUCS_KINDS_DUMP=<file>`).
- The throwaway tests are in `hunt-probes.patch`, `huntStreaming` in `hunt-probes-streaming.patch`, `huntSymlinkImage` in
  `hunt-probes-symlink.patch`
  (each applies alone to `271f48c`): `git apply` one, build `test_markdown_doc` and `test_claude_code`,
  and run:
  - `huntStarve`, `huntEmptyImage` (B2), `huntFutureDate` (B5), `huntBigImages` (C1) and
    `huntManyImages` in `test_markdown_doc`;
  - `huntMathReply` (B1), `huntBackgroundRace` (B3), `huntPromptMath` and `huntStreaming` (B7) in
    `test_claude_code`.

  They print `HUNT` lines (`QT_QPA_PLATFORM=offscreen`).
- The fuzzers, as the morning's report gives them:
  - `fuzz_kinds.py process 2110 40`;
  - `fuzz_qfem.py` with `examples` limited to the six quantum models, seed 2111, 30;
  - `test_markdown_doc fuzz` (`QUCS_MARKDOWN_FUZZ=240`, `QUCS_MARKDOWN_FUZZ_SEED=2112`);
  - `fuzz_tools.py 2113 600`;
  - `test_gui_monkey` (`QUCS_MONKEY_SEED=21102`, `QUCS_MONKEY_STEPS=2500`).
