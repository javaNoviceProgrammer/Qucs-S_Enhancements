# The after-the-release hunt's findings, fixed

The user (2026-10-22): *"Fix all the findings, then commit and build the DMG"*, of
[the hunt after the release](../bug_hunts/2026-10-22-after-the-release.md): 21 findings and 16 notes,
from `27968ca`, `f11dc6b`, `60d0e66` and `22cd6c8` and some of 20 and 21 October. Each finding is fixed
below with its test, and so are the notes, but N14 (kept, as said) and N12 and N15 (a script and the
build, no test). Fixing them found four slips of the fixes' own first versions, each caught by a test
and put right (*Found while fixing* below), and one claim of the hunt's report wrong (B9).

## A. Wrong results taken without a word

**A1. The barriers' bulk.** The barriers' bulk absorption is each barrier's own: a run of one material
outside the wells, its gap, masses and Rydberg, over its thickness. Runs of one material are counted
once, with their thicknesses together, and the barriers' whole thickness is the line less its walled
wells, as before. The fade of the bound steps (`continuum: bound`) is at the lowest barrier's gap. All of
this used to be the material where the line starts. Now:
- A 9.4 nm GaAs well at the line's start gives the spectrum of the same well with its barrier first. At
  1.44 eV under `states` with the edge that is 0 (it was 14.5 ×10³/cm). Under `bound` at 1.60 eV it is
  7.62 ×10³/cm (it was 31.0).
- Barriers of Al0.3 and Al0.1 around an 8 nm well, mirrored, give the same spectrum: 6375 /cm at 1.72 eV
  under `bound` either way. It was 989 one way and 11,125 the other.

**A2. The barriers' edge under `states` only adds.** The excess bulk is Elliott's absorption with
Urbach's tail, less the free carriers' broadened by the spectrum's line (the box's states). At each energy
it is never below nought. Where the free part is more than Elliott's (below the gap under a wide line,
or far below it in a Lorentzian's tail), the edge adds nothing. Before:
- 2 states, a 30 meV line and 0.5 meV Urbach gave −468 /cm with no carriers. At 1.83 eV it is now
  852 /cm, the box's states', and the edge comes in from 1.84 eV.
- With a 2 meV Lorentzian line, the edge took 115 /cm off at 1.30 eV whatever the state count. With the
  edge on, 1.30 eV is now the same as with it off (165 and 238 /cm with 8 and 32 states; it was 50 and
  123).

The edge's strength against a slab of the barrier is unchanged: 0.8333 of it, the barriers' 50 of 60 nm.

**A3, A4. A 1D semiconductor read along its line.** The line's values are kept per segment, each end as
that segment's own material reads it (a heterojunction's node of the other semiconductor on this one's
bands, as a triangle's are). The quasi-Fermi potentials and the trap-assisted tunnelling's field are now
the line's, along with V and the carriers. Against the 2D reference:
- n-GaAs/n-Al0.3GaAs: n 1.5 nm into the AlGaAs is 2.31×10²⁰ /m³, the 2D's, within 1.5e-13. It was
  1.05×10²², 46 times too many.
- The silicon junction: φn agrees to 1e-15 between the coarse mesh's nodes. It was 0.389 V against 0.059.

**A5. Spectra compared by energy.** Each point's absorption is compared with the first point's at the
first point's energies, interpolated where its own differ and left out past them. The local index is made
at each point's own energies, and the warning says when they move with the sweep. A sweep whose energies
move gives the same local index as one at fixed energies, to 1e-9. By index, a point 0.1 eV higher had a
Δn of 0.037.

**A6. `conservearea` where the first point absorbs nothing.** Its area is not kept there: the index is
that of the difference as it is, and a warning says so. A window of 1.38 to 1.44 eV, F 0 then 40 MV/m,
now gives Δn 3.5×10⁻³. Every Δn used to be exactly 0.

**A7. Flags given as expressions, read where they are used.** `flagIn` reads a flag in the parameters'
scope wherever it is used, and one that is not a constant fails, said. It is used at:
- the mesh's `custom` and the geometry's `closed`, `keep` and `interior`;
- a process step's `selective`;
- a derived value's `revolved` and an export's `mixed`;
- a plot's `quality` and `ylog`, and the default plots' carriers;
- the Lumerical exports (all their flags, read with the parameters first);
- the solver's `follow` before a sweep.

`"custom": "c"` with c = 1 now gives 8746 triangles. It gave 717, its warning saying Customize was off.

**A8. A number as a flag.** A number is true where it is not 0. A number 1 (from `fem_edit` or a file)
was false, with nothing said.

**A9. An unlit device's noise through its load.** Behind a circuit, the noise through the load is made
with or without light. It was made under the light's modulation alone. A dark bar behind Rs equal to its
own resistance now gets `semi.SnL_2` = 9.29×10⁻¹⁹ A²/Hz, a quarter of its own 3.72×10⁻¹⁸: half its
current through the divider.

**A10. A field past the gap.** Where the lowest electron state lies below the highest hole state, a
warning says so: "in its field, 300 MV/m (3 V over the line's 10 nm), the lowest electron state lies
219.8 meV below the highest hole state - the gap gone across the line, each state held by its ends alone:
not a well's states, nor what they absorb (a field in kV/cm meant? 1 kV/cm is 0.1 MV/m)". Nothing is said
at 10 MV/m.

## B. Bugs

**B1. A wait outliving its model.** A model deleted while a call waits for its job (the application closes
every document as it quits, the client gone) tells each wait from the event loop once it is gone. The
call's own pointer then reads null: "The model was closed.". After answering, a wait reads the model's
pointer again before it counts itself off, since the answer may have closed the model. Two things went
wrong before:
- the count was taken off the freed model: a heap-use-after-free under ASan, at every quit of a client
  during a solve;
- the wait's timer, deleted with the model, told the call of a model half destroyed.

**B2. A stop stops what waits behind it.** Each stop (`fem_stop`, Cancel, the tab closed) is counted. A
`fem_solve` that waited for the model to be free does not start once a stop came meanwhile: "Not solved:
the work it waited behind was stopped (fem_stop, Cancel), and a stop stops what waits for it too;
fem_solve again to solve." `fem_stop` waits for the jobs running when it was called alone. On the hunt's
case, `fem_stop` answers in 0.03 s. Before, the queued solve started as the stopped job ended, and
`fem_stop` waited for it and said `"stopped": true` 4.3 s later, once it was solved.

**B3. A process flow's build not waited for by reads.** `fem_describe`, `fem_edit` and `fem_model`
(`new`, `set`, `import`) no longer wait for a process flow's geometry. They say "being built in the
background, 66 % - … (fem_build waits for it)", or "not built (fem_build builds it)", and nothing starts
a stopped build again. A stop (`fem_stop`, Cancel) also drops the build waiting to start: a change's on
the window's timer, one wanted after the one stopped, and the stopped one's for a model changed since.
`fem_describe` used to wait for the whole anneal, 9.1 s here and minutes under the sanitizers. After a
stop it built the flow again, and so did the change's timer.

**B4. The flags a solve reads.** A study's solve reads its own nodes' flags alone: not a disabled node's,
not the results' (read where they are used, A7), not another study's. A disabled step's `"excitons":
"maybe"` or a plot's `"ylog": "maybe"` failed every study.

**B5. Harmonics once a run.** The Harmonics derived value is listed once a point of a sweep, named by it,
not at each output time. `fem_solve` lists every value of one run (up to 200), H1 first, and the last 40
of a value at many times or points. 30 harmonics over 81 times were 4941 values, of which the last 40 were
shown, H1 and H2 not among them. Now they are 61 values: the mean, H1 to H30, HD2 to HD30 and THD.

**B6. The sum-rule warning names what changes.** Where the field is the same at each point (`qw.Fw`), the
warning says the sweep's own change: "the field the same at each: what the sweep changes changes it (the
carriers filling the states, the temperature, the excitons)". A sweep of the carrier density was told of
"a state's partners in a field".

**B7. A local window that makes nothing is said.** These cases are now said:
- a window without a sweep;
- one not of two energies ("wants two energies, it has 1"; a single one was quoted as `"0"`);
- one that is not a photon energy, wavelength or frequency;
- one holding fewer than three of the spectrum's energies: "holds 2 of the spectrum's photon energies
  (1.38 to 1.6 eV), three at least wanted - none made".

A window in nm (or a frequency) is read as such; it was taken as a number of eV, empty. Its ends are taken
in to rounding, so a window given at the spectrum's own energies holds them.

**B8. The panel and a flag given as an expression.** The panel shows such a flag as its text, to edit: a
line edit, its tip saying what it is. "true", "false", 1 or 0 typed makes it a box again. What is shown
only while a flag is on, or off, is shown for one given as an expression. "excitons": "ex" (on) showed
as an unchecked box, with the exciton model, its size and its pairs hidden.

**B9. `fem_evaluate`'s time and frequency.** A time or frequency that is not read, is of another unit,
is a plain number for a spectrum (a unit is wanted), or lies past the outputs is refused, and the error
says which:
- "frequency: "x" is not read: … - a photon energy, wavelength or frequency, with its unit ("1.5[eV]",
  "826.6[nm]") wanted";
- "frequency: "3[eV]" lies past its outputs: from E = 1.4 eV to E = 1.6 eV";
- "time: "1[V]" is not a time".

"1.5 eV" and "0.25 ns", as the answers write them, are read. A Valence Bands step's outputs are its k
points (in 1/m, or "0.5[1/nm]"), a Laser L-I step's its biases. Each of these used to give the last
output, or the nearest end, without a word, and a plain number was taken as hertz. Plots and exports
that pick a time or frequency say the same.

*The hunt's report said the energy taken was named nowhere: the answer's `solutions` names it (the
report's probe printed the values alone). Corrected there.*

## C. Time

**C1, C2. Long Markdown documents.** Three causes, each fixed:
- **The import.** The Markdown is read into a document of no view, then put into the preview's at once,
  one edit, after it has been cleared so a changed image file is read again. Read into the preview's
  document, Qt laid each block out as it came and a long one again at each of its pieces, and the scroll
  bars' coming laid it all out again.
- **Long runs.** A run of more than 64 letters with no break opportunity (letters, digits, braces) gets a
  zero-width space every 64 letters, put in once a fragment. Copying from the preview takes them out.
  Where a quote or a list narrows the column, Qt broke each line anywhere after searching the run to its
  end: a letter a line, in time quadratic in the run's length. The plain Qt browser does the same, 8.4 s
  for 32 kB in a 160 px column. Put in one at a time, the breaks cost the block's length each: 3.7 s for
  128 kB with no layout at all.
- **Images.** The images' sizes are fitted in one edit. Each image's size set laid its long block out
  again.

| Document | Before | Now |
|---|---|---|
| `d161`: a list item and 105 kB of display math's braces | 24-39 s | 1.1 s |
| `d283`: 116 kB of `$` and brace runs | 67 s | 0.17 s |
| `d697`: 7 lines, a hundred images after a backtick | 3.6 s | 0.09 s |
| `d1030`: 50 quotes over 76 kB of `$\sqrt` | 8.5 s | 0.62 s |
| 32 kB of letters three quotes deep | 8.9 s | 0.48 s |
| 128 kB of letters in a paragraph | 3.2 s | 0.26 s |

## Notes

- **N1.** The spectrum's messages, descriptions and the sum-rule warning name its physics by its own tag:
  "qd: the absorption's sum…", "(qd.dnTE, qd.dnTM)". It was "qw." whatever the tag, and "qw.:".
- **N2.** `pmin` is shown in the panel under `continuum: bound` too, where it decides the held states.
  Its tip says both uses.
- **N3.** `QucsControl::waitsFor` knows `wait_for` (60 s), `simulate` (120), `git_remote` (300) and
  `git_clone` (600) by their defaults. `callNow` waits that long.
- **N4.** `fem_evaluate` with `all` and a time or frequency gives that one at each point. It gave every
  output: 174 values for one asked.
- **N5.** "True", "FALSE" and the like are flags as such, in any case. "True" had been refused since
  `f11dc6b`.
- **N6.** Harmonics:
  - a count outside 1 to 50, or periods below 1, is refused (they were clamped silently, a count of 0
    giving H1);
  - harmonics past what the output times resolve are said: "past H19 its output times, 25 ps apart,
    resolve nothing (half their rate): H20 to H30 aliased";
  - a signal not periodic over its window is said: "its ends 13 % of its swing apart (a start's
    transient not settled, or an f0 not the signal's own …) - its harmonics leak";
  - the help says the compression is a plot to make, not a value computed.
- **N7.** `semi.Sn_` and `semi.SnL_` are shown in A²/Hz (they were "s·A²"), `semi.in_` in A/√Hz and
  `semi.NEP_` in W/√Hz (they were plain numbers).
- **N8.** A hidden trace is said `"hidden": true`, not "not drawn" for a layout's failure. A marker on it
  says "it is hidden (edit_trace 'hidden': false shows it)".
- **N9.** A semiconductor that fails where Eg/2kT passes 150 says so: "- at 1 K its materials'
  intrinsic density is e^(-6762) of their states' (Eg/2kT): drift and diffusion's carrier statistics give
  out at such a temperature (a junction of silicon or GaAs solves at 77 K, not at 40)". A silicon or GaAs
  junction solves at 77 K and fails at 40.
- **N10.** A 1D line's `at` that is not finite is said as such: "it is inf, not a finite number".
- **N11.** `fem_plot` of a global over a solution's outputs makes a 1D plot group, the global graphed
  against them. It drew a 2D surface at the last output, one colour.
- **N12.** The 21 October kinds fuzzer keeps a refusal's text apart (`said`), so a kept slow model's file
  is the model. *No test: a script.*
- **N13.** A Mode Profile of fewer than two points a side (or more than 4000) is refused, said: "1 by 101
  points: a mode's profile is a cross-section - from 2 to 4000 along x and y (a Field on a Grid exports
  one column or row)". It was made 2 without a word. Taking one column, as the hunt suggested, would have
  made a file the Mode Overlap cannot read back: its reader wants two by two.
- **N14.** Kept: once its input ends, the stdio server answers the calls under way before it quits. A
  piped client (`echo … | qucs-s --mcp-server`) closes its input at once and still wants its answers. B3
  takes out the case that held it longest, a `fem_describe` waiting for a flow's build (259 s on ASan).
- **N15.** The application links the tests' AddressSanitizer options (`tests/sanitizer_options.cpp`,
  `736c82a`): eight frames an allocation's stack on macOS, so a tool's process forked by the sanitized
  application does not die in `fork()` on the depot's lock. The file is empty in Release and on Linux.
  *No test: the build's.*
- **N16.** The held states are said with each one's share in its well, the barriers' own count beside,
  and the threshold's weight: "so bound where their well holds 0.6 of their |ψ|² at least ("pmin"),
  beside the 2 the barriers bind: held e4 (0.71), e5 (0.52), e6 (0.6); not held e3 (0.3). Which are taken
  in hangs on "pmin" between their shares, and the absorption with it".

## Found while fixing

- **B9's first version** refused a Valence Bands step's plain k (`"frequency": "5e8"`, its outputs k in
  1/m) as not a photon energy. `claudesToolsShowTheWellsInEV` caught it. Those outputs are now read as k.
- **A5's first version** made a moved point's local index at the first point's energies and put it on the
  point's own. The new test, against a fixed-energy sweep, caught it. The index is now made at each
  point's own energies.
- **A2's first version** broadened both parts of the excess by one line. That moved the normal case 2.4 %
  below the slab's (`theBarriersEdgeUnderStates`, 0.8129 for 0.8333). The clamp at nought replaced it.
- **A1's first version** took the barriers' thicknesses from the line's points alone, which missed its
  ends (the same test). They are now shares of the barriers' whole.
- **C1's first version** put the new content over the old without clearing the preview's document. A
  changed image file was then not read again (`imagesWrittenAgainRenderAgain`, `imagesDecodedOnce`).
- **B3's test** found the window's own rebuild after a change starting again after `fem_stop`, and a
  stopped build of a superseded model restarting. A stop now drops both.
- **B7's test** found a window given at the spectrum's own energies missing one of them to rounding. The
  window's ends now take it in.

## Tests

| Finding | Test |
|---|---|
| B1 | `test_multiphysics_doc aWaitOutlivesItsModel`: the model deleted in the answer to its wait, told once, the wait's count off a model gone; deleted while a `fem_build` waits, "The model was closed." once |
| B2 | `test_multiphysics_doc aStopStopsWhatWaitsBehindIt`: a junction swept over 400 biases, a queued `fem_solve` told it was stopped, `fem_stop` in under 5 s, nothing started after; the same with Cancel; a solve asked after the stop its own, `fem_stop` answered before it |
| B3 | `test_multiphysics_doc aFlowsBuildIsNotWaitedForByReads`: a 600 min anneal, `fem_describe` (both kinds) and `fem_edit` answered within 5 s, the build said; after `fem_stop`, "not built" and nothing built again |
| B5 | `test_multiphysics_doc harmonicsListedOnceARun`: 61 values, H1, H2, THD and the mean among them, none at a time; its note's aliasing |
| B9, N4, N11 | `test_multiphysics_doc outputsAskedAsAnswersSayThem`: "1.45[eV]", "1.45 eV", "855.1[nm]" read; "x", "1.5", "1[V]", "3[eV]" refused, said; `all` at one energy at each point; `fem_plot` of a global a 1D graph. `harmonicsListedOnceARun`: "0.5 ns" read, "5[ns]" and "1[V]" refused |
| B8, N2 | `test_multiphysics_doc thePanelShowsAnExpressionFlag`: `"excitons": "ex"` a line edit, the exciton model and `pmin` shown; "true" typed a box again, checked |
| N3 | `test_multiphysics_doc aToolsOwnWaitIsWaitedFor`, extended |
| A7, A8, N5 | `test_fem_physics flagsOfTheParametersBuild`: `custom` and `closed` as "c", "c > 0", 1, "True", "TRUE" at c = 1 the fine mesh and the closed polygon, as "c", 0, "False" at c = 0 not; "maybe" said |
| A8, N5, B4 | `test_fem_exciton flagsOfTheParameters`, extended: 1, "True" and "1" on, 0 off; a disabled step's and a plot's "maybe" not this solve's |
| A1, A2 | `test_fem_exciton theBarriersEachTheirOwn`: mirrored barriers of two alloys alike under each continuum; a well at the line's start as at its end; a wide line's spectrum never below 0; a Lorentzian's at 1.30 eV the same with the edge |
| A3, A4, N9, N10 | `test_fem_modulator semiconductorIn1DAcrossAHeterojunction`: n and φn across GaAs/AlGaAs to rounding of the 2D's; `"at": "1/0"` said; 1 K said; `semiconductorIn1D`: φn and φp between the coarse mesh's nodes |
| A5, A6, B6, B7, N1 | `test_fem_quantum spectraComparedByEnergy`: a moving sweep's index the fixed one's to 1e-9, said; a window the first point does not absorb in, Δn not 0, said; a density sweep's warning without a field; a window without a sweep, of one energy, too narrow, past the spectrum said; one in nm made; a physics tagged `qd` named so |
| A10 | `test_fem_quantum aFieldPastTheGapSaid`: at 300 MV/m said, at 10 MV/m not |
| N16 | `test_fem_quantum aBoundStatesStrengthInAField`, updated: the shares and the barriers' count |
| A9, N7 | `test_fem_photodiode_power thermalNoiseOfABar`, extended: behind Rs = R a quarter through the load; A²/Hz, A/√Hz |
| N6 | `test_fem_photodiode_power harmonicsOfABar`, extended: 30 harmonics' aliasing said from H20; f0 0.7 GHz's leak said; counts 0 and 51 and periods 0 refused |
| N8 | `test_diagram_tools aHiddenTraceIsSaidHidden` |
| N13 | `test_fem_modeoverlap aProfileExportedAndReadBack`, extended: one column refused, said |
| C1, C2 | `test_markdown_doc longRunsRenderInLinearTime`: a run three quotes deep, four times as long in less than eight times the time (Release); its breaks there, copied without them; 400 images in one block in less than eight times 100's |

**Breaks.** There were 49 breaks in Release and one under the sanitizers, each run as a script: the
fix undone, its target built, its tests run, the file put back. All were caught:
- B1: a model deleted under a wait not told (Release); the wait reading the model before the answer
  (ASan's heap-use-after-free);
- B2: a queued solve started after a stop; `fem_stop` waiting for any job;
- B3: `fem_describe` waiting for the build; a stop leaving the build waiting to start;
- A7: the mesh's and the geometry's flags read as flags alone; A8: a number false; N5: "True" not a
  flag; B4: disabled nodes and the results read for a solve;
- B8: an expression flag as a box; shown-if by the text; N2: `pmin` for `localized` alone;
- A1: the barriers the line start's material; the fade at the line start's gap; A2: the excess below
  nought;
- A3: the line one value a node; A4: the quasi-Fermi potentials from the nodes;
- A5: spectra by index; A6: scaled to nothing; B6: always a field; B7: no word without a sweep, a window
  in nm as eV, a narrow window unsaid; N1: "qw." back in a message;
- A9: the load's noise under light alone; N7: `in_` a plain number; A10: no word of the gap gone;
  N16: none held unsaid;
- B5: Harmonics at each time; one run's values cut to 40; N6: the count clamped, the aliasing unsaid, the
  leak unsaid;
- B9: unread text the last output; "1.5 eV" not read; N4: `all` ignoring the frequency; N11: a global as
  a surface;
- N8: a hidden trace "not drawn"; a hidden trace a layout's failure; N9: a low temperature unsaid; N10:
  `at` "1/0" as no extent; N13: one column clamped; N3: `wait_for` unknown;
- C1, C2: read into the preview's document; no breaks in a run; the breaks copied; the images fitted one
  at a time.

One was not caught at first: **`fem_stop` waiting for any job** (B2). The test's later solve was asked
after `fem_stop`, so `fem_stop`'s wait ran first as the job ended. The test now has another caller whose
wait came first start a solve as the stopped job ends, as the queued `fem_solve` did in the hunt. Broken,
`fem_stop` answers only after that solve (24 s); fixed, as the job it stopped ends.

*Not tested:* A7's other sites (a derived value's `revolved`, an export's `mixed`, a plot's `quality` and
`ylog`, the Lumerical exports, a process step's `selective`, the solver's `follow`), which read the same
`flagIn` as the mesh and geometry tested; N12 (a script) and N15 (the build).

The whole suite passes in Release (135). The new and changed tests pass under ASan and UBSan, none
reporting. Each function takes 120 s at most under the sanitizers (`theBarriersEachTheirOwn`), the same
with Eigen's sparse solver, as CI's Linux uses. `harmonicsListedOnceARun` runs in Release builds alone,
as the other tests in time do. The CI lints pass.
