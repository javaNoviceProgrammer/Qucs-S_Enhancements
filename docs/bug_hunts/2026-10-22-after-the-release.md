# Bug hunt: after the release

*22 October 2026 - Qucs-S 27.0.3, `22cd6c8` (the Release build; the ASan/UBSan build where said).
Two hours, 06:24 to 08:24, in the foreground: hunting until about 08:05 (probes through the tools,
a throwaway test, six fuzzers), the write-up after. The findings are written up as they came; none
is fixed here. All fixed, the notes too but N14 (kept, said why), in the commit that adds
[docs/feature_gaps/2026-10-22-claude-after-the-release-hunt-fixes.md](../feature_gaps/2026-10-22-claude-after-the-release-hunt-fixes.md).*

The user's request: "continue hunting for bugs in our enhanced qucs for two hour and document your
findings ... If you find a bug don't stop to fix it". The last hunt
([2026-10-21-day-features.md](2026-10-21-day-features.md)) covered everything up to `271f48c`. This
one looks at what came after it:
- `27968ca`, that hunt's fixes: constants that are not finite, the asynchronous `fem_solve`, `fem_build`
  and `fem_stop`, the Markdown preview's image cache;
- `f11dc6b`, the Miller update check:
  - flags as expressions of the parameters;
  - the barriers' edge under `continuum: states`;
  - the ripple warning;
  - a sweep's start and retry;
  - a 1D semiconductor read along its line;
  - a one-column field export;
  - the `bound` pair rule and held states;
  - the local index (`dnwindow`, `conservearea`) and the sum-rule warning;
- `60d0e66` and `22cd6c8`, the CI fixes (`callNow` waiting as long as the tool does);
- from 20 and 21 October, not hunted yet: a contact behind a circuit, noise, the Harmonics derived
  value, thermionic heterojunctions, a hidden trace.

Each run had its own HOME, settings and cache in a scratch folder. The probe scripts and fuzzers are
in [2026-10-22-after-the-release/](2026-10-22-after-the-release/).

## Summary

| | Finding | Since |
|---|---|---|
| **A1** | The barriers' bulk absorption is the material where the line starts: a well at the line's start gets its own gap's bulk excitons (14.5 ×10³/cm below its first transition); mirrored asymmetric barriers differ 11-fold under `bound` | `f11dc6b` (`states`), before (`bound`) |
| **A2** | The barriers' edge under `continuum: states` takes off more than the box gives: absorption −468 /cm with no carriers (2 states, a 30 meV line); with a Lorentzian line 115 /cm off half an eV below the barriers' gap at 32 states | `f11dc6b` |
| **A3** | A 1D semiconductor read along its line interpolates the carriers across a heterojunction's jump: n 46 times too high 1.5 nm into the AlGaAs | `f11dc6b` |
| **A4** | Read along the line, the quasi-Fermi potentials still come from the mesh's nodes: φn 0.389 V against the 2D reference's 0.059 V, where V, Ec and n agree to rounding | `f11dc6b` |
| **A5** | The local index (and the sum-rule check) compares each point's spectrum with the first's by index: photon energies that move with the swept parameter give Δn of 0.037 | `f11dc6b` |
| **A6** | `conservearea` over a window the first point does not absorb in scales every other point to nothing: Δn exactly 0 although α rose to 960 /cm there | `f11dc6b` |
| **A7** | A flag as an expression is read for the solve alone: a mesh's `custom`, a geometry's `closed`, `keep` and `interior`, a derived value's `revolved`, an export's `mixed` and a plot's `quality` take it as false (717 triangles for 8746) | `f11dc6b` |
| **A8** | A flag given as the number 1 is false; the text "1" is true | before |
| **A9** | An unlit device behind a circuit: its noise through the load (`semi.SnL_`) is never made | `7d0cb11` |
| **A10** | A field whose drop over the structure passes the gap: the e1-hh1 transition below zero (−0.22 eV at 300 MV/m on 10 nm, −5.6 eV at 1 GV/m), the spectrum given without a word | before; found by the fuzz |
| **B1** | A crash: a call waiting on a model (`fem_solve`, `fem_stop`) answered as the app quits - the client gone - and `whenJobEnds` then counts down on the deleted model: heap-use-after-free under ASan (abort) at `multiphysicsdoc.cpp:1496`, every time; met twice by the hunt | `27968ca` |
| **B2** | `fem_stop` while another caller's `fem_solve` waits: the stop lets the waiting solve start, waits for that one too, and says `"stopped": true` after it ran to its end | `27968ca` |
| **B3** | `fem_describe` of a model whose process flow is being built waits for the whole build in a nested loop (9.1 s; 60 s sanitized), and after `fem_stop` builds it all again | `da357b4`; the pattern `27968ca` B6 removed elsewhere |
| **B4** | A malformed flag on a plot group or a disabled node fails every study: "its "ylog" is "maybe"" | `f11dc6b` |
| **B5** | The Harmonics value, one per run, is listed at every output time: `fem_solve` counts 4941 values and shows the last 40, H1 and H2 not among them | `93cb837` |
| **B6** | The sum-rule warning blames a field on a sweep that has none: a carrier-density sweep is told "a state's partners in a field" | `f11dc6b` |
| **B7** | `dnwindow` without a sweep, in wavelengths, past the spectrum or two energies wide: nothing made, nothing said | `f11dc6b` |
| **B8** | The Multiphysics panel shows an expression flag as an unchecked box, and hides the properties shown only when it is on | `f11dc6b` |
| **B9** | `fem_evaluate` at a `frequency` or `time` it cannot read ("1.5 eV", "0.25 ns", as its answers write them) gives the last one's value; one of another unit is used as is, one past the run the nearest end's. Only the answer's `solutions` shows which was taken | before (`8785624` for energies) |
| **C1** | The Markdown viewer: a list item followed by a long line of display math takes 24 to 67 s to open (the window frozen), quadratic in the line's length | before; found by the fuzz |
| **C2** | The Markdown viewer: a long run of text with no break (letters and digits) takes quadratic time to lay out: 128 kB in a paragraph 3.2 s; 32 kB three quotes deep 8.9 s. Likely C1's cause too | before; found by the fuzz |

The six fuzzers found B1 (the spectrum fuzzer's server, its run stopped), A10, C1 and C2, and met A2, A3
and B3 again; otherwise no crash, hang or sanitizer report:
- **tools:** 8,132 calls of every tool but those that run commands, on the ASan build;
- **fem tools:** 540 calls of the `fem_*` tools on a semiconductor, a quantum well and a process flow
  (ASan); its hangs are B3's;
- **Markdown:** 300 hostile documents on ASan and 1,449 in Release, each timed; C1 twice, C2 once
  (`d1030`, 8.5 s), and one 11 kB document 3.6 s to open whose parts each open at once;
- **node kinds:** the 21 October kinds fuzzer again, 360 semiconductor devices and 400 process flows,
  its one finding B3's wait (66 sanitized solves past its 20 s mark, none hung);
- **spectrum physics:** the quantum spectrum's options, 1,469 cases in Release (negative and
  non-finite absorption checked: seven negative, all A2's: `states`, the barriers' edge, a Lorentzian
  line) and 122 sanitized (five answers with NaN, all qw.Eb or qw.lambda, which are NaN by design; one
  of them A10);
- **1D lines:** 200 random stacks read along their line against a 2D reference: two off, A3's at an
  interface, and one 1.9 mV of 0.89 V inside a layer of an AlGaAs n-p-n at 0.2 V, not pursued.

## A. Wrong results taken without a word

### A1. The barriers' bulk is the material where the line starts

The bulk absorption that stands for the barriers is built from the point where the line starts: its gap,
its masses, its Rydberg. That holds for `continuum: bound` (its fade and bulk) and, since `f11dc6b`, for
the barriers' edge under `states`. It is right when the line starts in a barrier and all barriers are
alike. Otherwise:
- **A well at the line's start** (`f4.py`: 9.4 nm GaAs, then 16 nm Al0.3GaAs).
  - Under `states` with `bulkexcitons` it adds GaAs's own bulk excitons over the barrier's thickness:
    14.5 ×10³/cm at 1.44 eV, below the well's first transition, where `states` alone gives 0.
  - Under `bound` the well's steps fade out above GaAs's gap and bulk absorption replaces them: 31.0
    ×10³/cm at 1.60 eV, against 7.6 with the barrier first.
- **Asymmetric barriers** (`f35.py`: Al0.3 below, Al0.1 above an 8 nm well), `bound` at 1.72 eV:
  - 989 /cm with Al0.3 first: the Al0.1 barrier's own absorption from 1.55 eV is missing;
  - 11,125 /cm mirrored: Al0.1's bulk is counted over both barriers' thickness;
  - `states` gives 5930 either way.

### A2. The barriers' edge under `states` takes off more than the box gives

Under `continuum: states` the barriers' edge adds Elliott's absorption with Urbach's tail, less the free
carriers' √ε broadened by the spectrum's line. The box's states are supposed to supply that free part.
When they do not, the subtraction is left uncovered (`f18b.py`, `f18c.py`, `f25.py`).

**A narrow edge, a wide line.** A 2 nm well in 50 nm barriers, 2 states, a 30 meV Gaussian, Urbach
0.5 meV, no carriers:

| eV | 1.80 | 1.81 | 1.82 | 1.83 | 1.84 |
|---|---|---|---|---|---|
| `states` | 666 | 702 | 764 | 852 | 960 |
| with the edge | 490 | 315 | 5.9 | **−468** | 13,944 |

**A Lorentzian line.** Its tail carries the subtracted free part far below the barriers' gap. On the
spectrum fuzzer's case (`ph_54_57`: a 2 meV Lorentzian), α(1.30 eV) with the edge on:
- 1 state: −67 /cm;
- 8 states: 50 /cm, against 165 without the edge;
- 32 states: 123 /cm, against 238 without the edge.

So turning the edge on takes some 115 /cm away half an eV below the barriers' gap, whatever the state
count. It should only add, and only near that gap.

### A3. Read along the line, the carriers are interpolated across a heterojunction

**Setup.** n-GaAs/n-Al0.3GaAs, 1e17 cm⁻³, 0.1 V, solved in 1D (`h1d` 2.5 nm) on a 70 nm mesh, against
a layered 2D reference (`f37.py`).

**Result.** V, Ec and n agree to rounding at every point but one. 1.5 nm into the AlGaAs, within the
line's first element past the interface, n reads 1.05×10²² against 2.31×10²⁰ per m³: 46 times. The
line keeps one value at the interface node, the GaAs side's, and the reading interpolates the jump from
it. Before `f11dc6b`, the per-triangle heterojunction shift corrected the carriers at a point; the
line's value now replaces them after that shift.

**The 1D-line fuzzer (`fz_line.py`)** met it again: 13 % at 0.5 nm from a GaAs/Al0.3GaAs interface, in
a depleted stack.

### A4. The quasi-Fermi potentials still come from the mesh's nodes

**Setup.** The 1D junction of the test `semiconductorIn1D` (silicon, 0.5 V, a 70 nm mesh) against its
2D reference, at 31 points between the coarse nodes (`f6.py`).

**Result.** V, Ec and n agree to 1e-15, but semi.phin and semi.Efn do not: φn is 0.389 V against
0.059 V at y = 15.8 nm. `semiconductorAt` takes V, n and p from the line but builds the quasi-Fermi
potentials from the triangle's nodes. So at one point φn, V and n disagree with each other by
e^(0.33/0.026). The trap-assisted tunnelling field is the nodes' too. The warning says the potential,
carriers "and what follows from them" are read along the line.

### A5. The local index compares spectra by index, not energy

**Setup.** A sweep of specified pairs (F 0 and 10 MV/m with E0 1.38 and 1.48 eV), the energies
`linspace(E0, E0 + 0.57[eV], 58)` (`f2.py moving`).

**Result.**
- The second point's spectrum lies 0.1 eV higher. `spectraAgainstTheFirst` takes its α as on the first
  point's energies: the local Δn reads 0.0367 at 1.49 eV, where a field gives 10⁻⁴ to 10⁻³.
- The sum-rule check does the same: "1.48 of the first point's".
- Nothing says the energies differ.

### A6. `conservearea` over a window the first point does not absorb in

**Setup.** A window of 1.38-1.44 eV, below the exciton; F 0, then 40 MV/m (`f11.py`).

**Result.**
- At the field, α(1.42 eV) is 960 /cm, against 0 at zero field.
- With `conservearea`, every point's α in the window is scaled to the first point's area there,
  nought. So every qw.dnTMloc is exactly 0, with no word; without it, 3.5×10⁻³.

### A7. A flag as an expression is read for the solve alone

`f11dc6b` reads flags given as expressions on a copy of the model made for the solve. Everything built
or read from the model itself still takes `Node::flag()`, which is false for any expression:
- the mesh's `custom` (`fem_mesh.cpp:1524`);
- the geometry's `closed`, `keep` and `interior` (`fem_geometry.cpp:636, 994, 1154, 1166`);
- a derived value's `revolved` (`fem_results.cpp:1124`);
- an export's `mixed` (`fem_export.cpp:983`);
- a plot's `quality` (`multiphysicsdoc.cpp:1794`).

With `"custom": "c"` and c = 1 (`f1.py`), the mesh has 717 triangles against 8746 with `true`. Its
warning even says "Customize is off".

### A8. A flag given as the number 1 is false

`fem_edit` sets `"excitons": 1` (a JSON number), and so may a hand-written file (`f3b.py`).
`Node::flag()` reads a number as false, and `expressionFlags` looks at texts alone. So no excitons are
computed, without a word, while the text "1" is true.

### A9. An unlit device's noise through its load is never made

**Setup.** A silicon bar behind a series circuit (`circuit: series`, Rs or RL equal to the bar's own
resistance), with `noise`, at 1 MHz (`f27.py`).

**Result.** semi.Sn_2 is the bar's own short-circuit noise, 4kT/R, and correct. But semi.SnL_2, the
noise through the load that the help promises "through a circuit's load", is never made. It is
computed inside `if (!a_Hop.empty())`, the light's modulation (`fem_semiconductor.cpp` 6772-6807): a
dark device behind a load, thermal or a dark current's shot, gets none.

### A10. A field past the gap's drop: transitions below zero, said nowhere

The spectrum fuzzer's case `fz_2_8` (a 9.4 nm GaAs well in 0.3 nm Al0.1GaAs barriers, 10 nm in all)
swept the field to 1 GV/m. Its answer had no word of it, only the Kramers-Kronig window's (`f72.py`):

| F | 0 | 100 MV/m | 300 MV/m | 1 GV/m |
|---|---|---|---|---|
| qw.Ee1hh1 | 1.489 eV | 1.077 eV | **−0.220 eV** | **−5.557 eV** |
| qw.alphaTE at 1.4 eV | 94 /cm | 1375 /cm | 2825 /cm | 2269 /cm |

At 300 MV/m the drop over the box, 3 V, passes GaAs's gap. The first electron state then lies below the
first hole state (E_e −2.47 eV, E_hh +3.09 eV at 1 GV/m), each held only by the box's ends. The
absorption is still computed from these states and given. A field this strong is a typing slip in
practice (MV/m for kV/cm); a negative transition energy, or a drop past the gap, could be refused or at
least said. The quantum code has no field warning at all.

## B. Bugs

**B1. A crash: a waited-for call answered as the app quits** (`f52.py`, `f52_asan_report.txt`; the
spectrum fuzzer's case `fz_1_8` and the crash report of 06:30:46).

How it happens:
- Through `qucs-s --mcp-server`, a `fem_solve` is sent and the client goes (its input closed) while the
  solve runs.
- When the job ends, `whenJobEnds`'s `end(true)` (from `stateChanged`) calls `told()`, which answers
  the tool. The client being gone, the application closes and the model is deleted meanwhile.
- `end()` then runs `--doc->a_waiting` on it: `doc` is a raw pointer taken from the `QPointer` before
  `told()`.

Under ASan that is a heap-use-after-free at `multiphysicsdoc.cpp:1496`, and the app aborts (exit −6).
It happened with the client leaving 0.3, 1, 2, 3 and 5 s into the solve. The Release build exits 0
here, the memory read regardless.

`fem_stop`, which waits through the same `whenJobEnds`, crashes the same way (`f53.py stop`).

The hunt met it twice:
- the spectrum fuzzer's server, when its first run was stopped at 06:30;
- the reproduction.

Any client that disconnects while a solve is waited for (a Claude Code session ended, a script killed)
does the same. `whenJobEnds` came with `27968ca` (B6).

**B2. `fem_stop` undone by a waiting `fem_solve`, and said stopped** (`f13.py`).

The calls, sent at once to one model:
- 101 `fem_solve`;
- 102 `fem_solve`;
- 103 `fem_build`;
- 104 `fem_stop`;
- 105 `fem_solve`;
- 106 `fem_evaluate`.

What happens:
- The second `fem_solve`, finding the model busy, waits up to 1 s for the job to end
  (`whenJobEnds(1000, ...)`).
- `fem_stop` cancels the first solve, so its caller is told "the mesh: cancelled". The cancel counts as
  the job's end, and the waiting `fem_solve` starts a fresh solve at once.
- `fem_stop` waits for that one too. At 4.4 s, as it completes with a full solution, `fem_stop` answers
  `{"stopped": true}`.

The window's Cancel button is the same `cancelJob` (`multiphysicsdoc.cpp:446`), so a solve another caller
queued undoes the user's Cancel too.

**B3. `fem_describe` waits for a process flow's build** (`f43.py`, `f45.py`; the kinds fuzzer's
`slow-process-71-32`).

What happens:
- `fem_build` with `timeout` 1 leaves a 20-hour anneal's flow building at 66 %.
- `fem_describe` then takes 9.1 s in Release (60 s sanitized): `MultiphysicsDoc::geometry()` waits for
  the build in a nested loop ("waited for with the window going on"). That is the pattern `27968ca` B6
  took out of `fem_solve`, `fem_build` and `fem_stop`.
- After `fem_stop` cancelled the build ("Build Geometry builds it, as does a change of the model"),
  `fem_describe` builds the whole flow again and waits 10.4 s.

The fem tools fuzzer's `fem_describe` and `fem_build` on a flow ran past its 90 s watchdog on ASan for
the same reason.

**B4. A malformed flag on a plot or a disabled node fails every study** (`f14.py`, `f22.py`).

Flags are read over the whole model but other studies, results and disabled nodes included:
- a plot group's `"ylog": "maybe"` fails every study: "The study failed: x: its "ylog" is "maybe"";
- a disabled spectrum step's `"excitons": "maybe"` fails its study;
- so does a disabled physics' `"light": "maybe"`.

The message names the node but not that it is disabled.

**B5. Harmonics listed at every output time** (`f24.py`).

The Harmonics derived value is one per run (or sweep point), computed from all of a solution's times. The
derived values in `fem_solve`'s answer list it at each output time:
- 3 harmonics over 81 times: 567 values;
- 30 harmonics: 4941 values, "the last 40 here". Those are H21-H30 and the dBc figures of the last time;
  H1 and H2, the ones wanted, are not shown.

A wrong period count is said as "t = 0 s: Harmonics: its 5 periods ...". (`fem_evaluate h3.H1` gives the
one value.)

**B6. The sum-rule warning blames a field that is not there** (`f2.py density`). A sweep of the carrier
density 0 to 3e18 cm⁻³, no field, is told "... is 0.761 of the first point's (TE): its strength not kept
- a state's partners in a field past the subbands taken". A sweep of the density, the temperature or the
excitons changes the area legitimately, and the warning is the field's.

**B7. A local window that makes nothing says nothing** (`f2.py nosweep`, `nm`; `f77.py`).
- `dnwindow` on a spectrum without a sweep: no qw.dnTMloc, no word.
- `dnwindow: ["850[nm]", "880[nm]"]`: a length taken as a number of eV, the window empty, no index and
  no word.
- On energies 10 meV apart (1.38 to 1.60 eV), a window of 1.45 to 1.46 eV (two energies) or one wholly
  above them (2.5 to 3 eV): no index, no word. A window wider than the spectrum is clipped to it and made.
- A window turned about, or of no width, is said. One of a single energy is said as `"1.38[eV]" to "0"`,
  the missing end quoted as 0.

**B8. The panel shows an expression flag as off** (code read, `multiphysicspanel.cpp`).
- Line 1389: the checkbox is `node.flag(key)`, false for "ex". So `"excitons": "ex"` (on) shows
  unchecked.
- Line 1143: what is shown only when a flag is on (`shownIf "excitons=true"`: the exciton model, its
  size, its pairs) compares the text with "true". So those properties are hidden while excitons are on.

**B9. `fem_evaluate`'s photon energy or time: whatever it cannot read, the last** (`f81.py`, `f82.py`).

A spectrum on 1.40, 1.45 ... 1.60 eV, `fem_evaluate` of qw.alphaTE with `frequency`:

| `frequency` | value | the energy taken |
|---|---|---|
| `1.45[eV]` | 415.5 /m | 1.45 eV, right |
| `826.6[nm]`, `1.5eV` | 8.07×10⁵ /m | 1.50 eV, right |
| `1.5 eV`, `x` | 1.27×10⁶ /m | 1.60 eV, the last: not read, not said |
| `3[eV]` | 1.27×10⁶ /m | 1.60 eV, the nearest: past the spectrum, not said |
| `1.5`, `1.5e-3[V]` | 0 /m | 1.40 eV: taken as 1.5 Hz and 1.5 mHz, not said |

`instances` (`fem_results.cpp:1702`) reads the text as a frequency, then a wavelength, then an energy.
When none reads, the last snapshot stands, and a value of another unit is used as hertz. Nothing says
the text was not read; the answer's `solutions` ("E = 1.6 eV") shows which energy was taken, for one
who compares it with what was asked. (*Corrected while fixing it: the first version of this report said
the energy taken was named nowhere; the probe printed the values alone.*) `"1.5 eV"` is how the tool's
own answers write an energy ("F = 0 V/m, E = 1.5 eV: qw.alphaTE").

`time` on a transient goes the same way (a bar's current, 0 to 0.5 ns): `0.25[ns]` and `250ps` give the
value at 0.25 ns; `0.25 ns`, `x` and `1[V]` give the last time's, as do `5[ns]`, past the run, and
`0.25`, read as 0.25 s, without a word.

## C. Time

**C1. A list item, then a long line of display math: the Markdown viewer frozen** (fuzz `d161`, `d283`;
`f30.py`-`f34.py`, `f36.py`).

Opening a list item followed by a long line of display math, each span many braces, takes:

| spans | 87 | 175 | 350 |
|---|---|---|---|
| characters | 27 k | 53 k | 105 k |
| seconds | 1.7 | 6.3 | 24-39 |

- The same line without the list item opens in 0.6 s.
- The Release fuzzer's `d283` (a list item, 116 kB of `$` and brace runs) took 67 s, and an edit of it
  hung past 120 s.
- The list's depth does not matter, nor do short spans.
- A sample puts the time in Qt's layout (`QTextDocumentLayout::documentChanged`, `layoutBlock`) during
  `QTextMarkdownImporter::import`, from `markdown::setMarkdown` in `MarkdownDoc::renderNow`.
- A throwaway test (reverted) found `findMath`, `htmlBalanced`, the typesetting and `setMarkdown` on a
  document of no view all take 0-2 ms. The viewer imports into its laid-out document.

The two files are kept: `slow_d161.md`, `slow_d283.md`.

The Release fuzzer met a third slow document without a list: `d697`, 11 kB, 3.6 s to open. Seven of its
lines (`slow_d697_lines23-29.md`, 5.5 kB) take it all. Without the first of them, a lone `$$`, or the
last, a hundred image links after a backtick, they open in 0.02 to 0.09 s. That `$$` changes how every
later dollar pairs. A display formula of image links, code or braces made by hand opens at once, so
what it pairs into was not narrowed further (`f61.py`-`f63.py`).

**C2. A long run without a break: laid out in quadratic time** (fuzz `d1030`; `f64.py`-`f70.py`).

The Release fuzzer's `d1030` (78 kB) took 8.5 s to open, and its picture 8.3 s. One line takes 6.5 s of
it: 50 nested quotes over 76 kB of `$\sqrt` with no space. The math does not matter:
- 4000 inline spans `$\sqrt$` open in 0.13 s at no depth, one quote deep, or in a list item;
- 6400 of them 12, 25, 50 or 100 quotes deep take 3.8 s alike;
- 6400 times `xsqrt`, 32 kB of letters: one quote 0.37 s, two 0.64 s, three 3.35 s, four to 50 8.9 s;
  with a space every four letters, 0.13 s.

The time grows with the square of the run's length, at any depth:

| letters and digits, no break | 32 kB | 64 kB | 128 kB |
|---|---|---|---|
| a paragraph | 0.31 s | 0.86 s | 3.19 s |
| a table cell | 0.30 s | 1.01 s | 3.60 s |
| a code block (not wrapped) | 0.05 s | 0.09 s | 0.17 s |

With `+` and `/` among them (base64), the paragraph breaks there and takes 0.25 s for 128 kB. A `.txt` of
the 32 kB run opens in 0.03 s. So a long word that must be broken anywhere costs Qt's layout time in the
square of its length. In a narrow column it is worse; presumably the quotes' indents leave the headless
window (528 pixels wide) a column of a few letters by the third level, so many more lines (not
measured). Such text is rare by hand, but a pasted blob with no break (a hex dump, a minified
identifier) is enough.

C1's lines have no break either (`{`, `$` and `\` throughout), and the list item and quote narrow the
column. Its time is in the same layout, so C2 is likely its cause. `d697`'s seven lines may be the same.

## Notes

- **N1.** Six messages and descriptions hard-code "qw." for the spectrum's variables (qw.dnTE,
  qw.alphaTEfar...) whatever its physics' tag (`fem_quantum.cpp` 4952, 4957, 5188-5196). The sum-rule
  warning begins "qw.:", the tag with its dot.
- **N2.** `pmin`, read under `continuum: bound` since `f11dc6b` (the held states), is shown in the panel
  only with `select: localized` (`fem_model.cpp:2960`; `shownIf` takes alternatives).
- **N3.** `QucsControl::waitsFor` knows the waits of `fem_build`, `fem_solve` and `fem_stop` alone, and
  another tool's when `timeout` is given. With `callNow`'s default 30 s, `wait_for` (60 s unless given),
  `simulate` (120 s) and the git tools (300 and 600 s) are still cut short ("no answer in time"); the
  late answer is harmless since `60d0e66`.
- **N4.** `fem_evaluate` with `all` and `frequency`: the frequency ignored, without a word. 174 values
  for one energy at each of three points (`f10.py`).
- **N5.** "True" (capital) as a flag, false before `f11dc6b`, is now refused: "'True' is not a
  parameter ... (did you mean true?)". A file that had it no longer solves.
- **N6.** Harmonics past what the samples resolve (H21 to H30 at 40 a period) are given without a word.
  An f0 the signal is not periodic in (0.7 GHz for 1 GHz) leaks (mean −483 mA). `count` is clamped to
  1-50 silently. The help's "compression" is a plot to make, not a value computed (`f24.py`).
- **N7.** semi.Sn_ is shown in "s·A²" (A²/Hz meant), and semi.in_ with no unit (A/√Hz) (`f27.py`).
- **N8.** `edit_trace hidden` on a diagram's last shown trace answers "not drawn: its diagram cannot lay
  out its axes over it (its limits)": the trace is hidden, not a layout failure (`f28b.py`).
- **N9.** A semiconductor at 1 mK fails as "the equilibrium's potential: a singular matrix", the
  temperature not named (0 K and below are refused).
- **N10.** `"at": "1/0"` on a 1D semiconductor is refused as "its domains have no extent along the
  line", not as not finite (`f51.py`).
- **N11.** `fem_plot` with `"expression": "qw.alphaTE"` draws the global as a 2D surface at the last
  energy, one colour ("3.4017e+06 to 3.4017e+06 1/m"), not against the photon energy (`f47.py`).
- **N12.** The 21 October kinds fuzzer (`docs/bug_hunts/2026-10-21-fuzz/fuzz_kinds.py`) reuses `text`
  for a call's refusal, so a slow call's kept `model.qfem` holds that refusal, not the model.
- **N13.** The Mode Profile export keeps its least grid of 2 by 2 while a Field on a Grid takes one column
  since `f11dc6b`.
- **N14.** A `fem_build` waiting on a process flow holds the application's quit until the flow is built:
  the client gone, it exits 9.9 s later, against 0.1 s when no call waits and the build is cancelled.
  On ASan it is minutes; a `fem_describe` waiting so (B3) held it 259 s (`f53.py build`, `f54.py`,
  `f56.py`).
- **N15.** On ASan one tool fuzz run left a crash report of a forked child killed in ASan's stack depot
  lock (`_notify_fork_child`, 06:38:24): the macOS ASan fork problem of `736c82a`'s note, from a tool
  that starts a process.
- **N16.** In a field that tilts every state past its lower barrier's edge (the shared 9.4 nm well at
  60 MV/m), the log says "8 e (0 bound), 8 hh (0 bound), 8 lh (0 bound) states", then "... held in their
  wells, so bound: e3, e4, e5, e6". The count leaves the held states out. The spectrum then hangs on
  `pmin` alone: α at 1.6 eV is 788, 647 and 145 /cm at `pmin` 0, 0.5 and 0.9 (e3 dropped at 0.5, all
  four at 0.9). Nothing says how much the threshold decides (`f80.py`; at 10 MV/m nothing is held and
  `pmin` changes nothing).

## Checked and found right

- **The sweep's start rule.**
  - On a geometry sweep (the n layer's thickness outer, the bias inner) every point equals the same
    point solved alone to 1e-14.
  - The older points kept for an outer step have lost only their walk's continuation, not their fields.
- **The 1D line readout.**
  - Along x as along y: V, n and Ex agree with the 2D reference to 1e-14.
  - Thermionic and tunnelling heterojunctions solve in 1D as in 2D (within 1.4-2 %).
  - About 100 random stacks: all within 1 % but the two slips above.
- **The local index.**
  - Repeated photon energies give finite values.
  - Energies listed shuffled or descending give the same index as sorted.
  - With gain in the window, `conservearea` stays finite.
  - Over a sweep it exports whole (12 energies by 3 fields).
- **Spectra.** Periodic ends with a field are refused. The 8x8 spectrum with excitons under `bound` takes
  67 s (81 s before the pair rule).
- **Circuits.**
  - Rs equal to a bar's resistance halves its current, an RL of three times quarters it, and a bias tee
    at rest takes Rs alone; a negative Rs is refused.
  - semi.V_ is the source's V0, as said, and semi.Vc_ the contact's own.
- **Closing during a solve.** A model closed during its solve and a build cancels both, with no
  sanitizer report.
- **Caches.** The Markdown image cache keys on the metadata change time too; every `QCache` insert
  stores a copy and keeps using its own.
- **The tools.** 8,132 calls of every tool but those that run commands, on ASan, with no crash or
  sanitizer report. Only `wait_for` waited out its default.
- **Quitting during a simulation.** The client leaving during a 20 ms transient of the example amplifier
  exits cleanly on ASan.
- **The node kinds.** The kinds fuzzer on ASan: 200 semiconductor devices and 400 process flows, no
  finding but B3's wait.
- **Markdown.** 300 hostile documents on ASan without a report, and 1,449 in Release; two were C1, one
  C2.
- **Reading a 1D semiconductor off its domains.** Beside, above and below the device, and just past its
  edge, nothing is given, as in 2D; at its edge the line's value (`f74.py`).
- **A sweep solved in parallel.** Two parameters, 12 points, `parallel` 2 and 4: the carriers equal the
  sweep in turn exactly (`f75.py`).
- **Flags of no finite value.** `"excitons": "0/0"`, `"1/0"`, `"-1/0"`, or a parameter that is NaN:
  refused, the flag and its value named. `"1e-320"` and `"1[V]"` are on, `"0[V]"` off (`f76.py`).
- **A local window wider than the spectrum** is clipped to it (`f77.py`).
- **`fem_evaluate`'s `point`.** Past a sweep's points it is refused ("There is no point 4: its sweep has 3
  points"); 2.7 and "2" are refused; 0 is the default, the last (`f83.py`).

## Reproducing

The probes ran through `qucs-s --mcp-server` with `mcp_client.py` (HOME, settings, cache and trash in
the scratch folder). They expect it at `/tmp/qucs-hunt` with the scripts in `/tmp/qucs-hunt/hunt2`, and
the builds at `/Users/meisam/git/Qucs-S_Enhancements/build` and `build-asan`; change those paths to
rerun.

| Script | What it does |
|---|---|
| `probe.py` | Opens a model and makes the calls; `f2.py` holds the quantum well the others share |
| `f1.py`, `f3b.py`, `f14.py`, `f22.py` | The flags: A7, A8, B4 |
| `f2.py` | B6, B7, A5 (`python3 f2.py density|nosweep|nm|moving`) |
| `f4.py`, `f35.py` | A1 |
| `f18b.py`, `f18c.py`, `f25.py` | A2 |
| `f37.py`, `f6.py` | A3, A4 |
| `f11.py` | A6 |
| `f27.py` | A9 |
| `f52.py`, `f52_asan_report.txt` | B1 (`QUIT_AFTER` seconds into the solve; the ASan build by default, another binary as its argument) |
| `f53.py`, `f54.py`, `f55.py`, `f56.py` | Quitting while `fem_stop`, `fem_build`, `simulate` and `fem_describe` wait (B1, N14) |
| `f13.py` | B2 |
| `f43.py`, `f45.py` | B3 |
| `f24.py` | B5 |
| `f30`-`f36` (`f32.py`, `f34.py`, `f36.py` kept), `f61.py`-`f63.py` | C1, with `slow_d161.md`, `slow_d283.md` and `slow_d697_lines23-29.md` |
| `f64.py`-`f70.py`, `slow_d1030.md` | C2: inline spans, `d1030` by lines, the quotes' depth, a run in a paragraph, a code block and a table cell |
| `f71.py`, `f72.py` | The sanitized spectrum fuzzer's NaN answers in Release; A10 |
| `f81.py`, `f82.py`, `f83.py` | B9; `point` |
| `f79.py`, `f80.py` | The spectrum's default plots (wavelengths, gain, a field); `pmin` in fields (N16) |
| `f73.py`-`f78.py` | A sweep's point at 10 kV, reading off a 1D device, a parallel sweep, flags of no finite value, B7's windows, the sum-rule check below the edge |
| `fz_tools.py`, `fz_tools2.py`, `fz_toolsF.py` | The tool fuzzers: type-confused; plausible with one hostile argument; the `fem_*` tools on three models |
| `fz_md.py`, `fz_mdr.py` | The Markdown fuzzers (ASan; Release, each call timed) |
| `fz_spec.py`, `fz_phys.py` | The spectrum fuzzers (ASan; Release, negative and non-finite absorption checked) |
| `fz_line.py` | The 1D line against the 2D reference |
