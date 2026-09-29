# Feature gaps: Qucs-S's MCP tools, the fifth and sixth rounds of feedback

*29 September 2026 — Qucs-S 26.1.4.*

Claude (Fable 5.1) wrote two reports on the morning of 29 September. The
fifth round (`qucs-mcp-api-feedback-round5.md`, at `474a0f5`) checked the
fourth round's list. It also ran seven end-to-end scenarios, which found
the worst bug of any round: `create_subcircuit` of parts in a row gave a
wrong voltage while every answer was green. The sixth round
(`qucs-mcp-api-feedback-round6.md`, at `e1e6fd4`) confirmed the partial
fix of that bug and found what was still left. It repeated five open
points of the fifth round and ended with a wishlist of eight items, in
order of value. Both reports were handled together. Each point was
checked against the source and reproduced before it was changed. The
tables say what was done.

## Bugs

| Feedback | Now |
|---|---|
| **R5 2.0, R6 2.1 `create_subcircuit` of parts in a row breaks the parent.** V1 → R1 → R2 → ground, C1 outside on the R1/R2 node. The wire R1.2–R2.1 stayed in the parent, and the instance, put at the group's centre, landed both pins on it: `XSUB1 mid mid`, v(mid) = 0 V (5 V with the wire bent), every answer green. `e1e6fd4` fixed the inside of the subcircuit only. | A wire that ends on two pins of the group goes with the group, whether or not its net has a port. Before, the pin a port "was at" kept its wire as the way to the outside. Each port's net in the parent is joined by a label on what is left of it: on the nearest wire end that stays, or on the outside pin when nothing stays. The instance goes where the group was, or else to the nearest place where no pin of it lands on another net's pin or wire. It searches rings out to 30 grid steps, first with room around the box, then without. The parent's nets are compared after: pin by pin, minus the group, plus the instance's pins on their ports' nets. If one differs, everything is put back, the subcircuit file too, and the answer says which pin would have gone where. `aDividerInARowKeepsItsNets` builds the report's three straight-wire variants: the label on R2.1, on C1's pin, and no label. Each gives `XSUB1` two different nets, V1 on the first and C1 on the second, a clean `check_schematic`, and v(mid) = 2.5 V under ngspice. |
| **R5 2.1, R6 2.2 `redo to=N` below the current step undoes.** "Undone 13 steps". | Refused: "Step 1 is before the current step 14: redo goes forward only - undo with 'to': 1 goes back there." `undo to` still goes either way, as it says. |
| **R5 2.2, R6 2.2 `tune`'s `measure` takes an unknown key.** `waht` measured the default. | `measure` lists its fields in the schema: variable, what, field, at, from, to, operating_point, level, tolerance, fundamental, harmonics, periods, decibels, form, simulator. The unknown-argument check now refuses `waht` with "Meant what?", as it does for the other nested objects. The new `knobs` and `targets` are checked the same way, and `check-tool-arguments.py` follows both into `tuneKnobs`. |
| **R5 2.3, R6 2.2 After a port swap, the save says undo puts the wiring back.** It does not: the symbol change is no undo step in the parent. | The advice says what does work: "The new symbol is no undo step there (an undo there takes away only the wires drawn on to the pins): delete the wires at those pins and connect each pin by its name (connect takes SUB1.in), or put the ports back as they were in the subcircuit and save it again." |
| **R5 2.4, R6 2.2 A `--workspace` run's scratch files go to the shared cache.** `spice4qucs.cir` and `log.txt` of every such run landed in `~/Library/Caches/qucs-s/qucs-s/`. | `--workspace` now always sets the scratch folder to `<workspace>/spice4qucs`, whatever the settings name. Before, it did so only when the settings named the old workspace's folder. `misc::cacheDir()` gives that folder to the project scratch and to `clean_scratch`, so a project's netlists go there too. `test_mcp_server`'s `theWorkspaceIsTheOneGiven` sets the settings' scratch to a shared folder and puts a stand-in ngspice on `PATH`. It then simulates a copied example: the `.cir` is in the workspace, and nothing is in the shared folder. |
| **R5 2.5, R6 2.2 `new_project name="."` answers "no slashes".** | Each refusal names its reason: "no letter or digit" (`.`, `{}`), "a slash", a character Windows refuses, "begins with a dot" (`.amp`: a hidden folder). |
| **R6 3 `import_netlist` with a `save_as` that exists keeps the document untitled**, says so only in a field, and has no `replace`. | Checked before anything is made, and refused ("… exists: 'replace' writes over it"); `replace` is in the schema and writes over it. |
| **R5 2.5, R6 4.8 The netlist map lists pins by number.** | A pin with a name is shown with it: `SUB1.1 (out)`. The map's `how` says so. |

## Wishlist (sixth round, section 4)

| Wish | Now |
|---|---|
| **1. Layout beyond `arrange`.** Place a part relative to another, route with a preferred side or waypoints, move label text, arrange feedback loops and supplies, tidy the wiring without moving parts. | `add_component` and `edit_component` take `near`: `{"part": "U1", "side": "below", "gap": 80}`, or just a part's name. The part goes on that side at that gap, lined up with the other part's centre. It is nudged along that side when its pins would land on something, and refused with `x` or `y`. `connect` takes `side` (`above`, `below`, `left`, `right`: the ways round that side are tried first, "Round the lower side.") and `via`, points to go through ("Through 300, 200; ..."), each leg routed as before. It says when no way through the points is free. `set_label` takes `text_at` for the label's text (it keeps its node). `arrange` takes three options. `keep_places` draws only the wiring again and leaves every part where it is: the tidy. `feedback: below` (or `above`, or `inline`, the default) puts an op-amp's feedback part under it. The part is turned to lie along the op-amp, with its pin under the outermost pin it joins (the output), so its wires stay off the symbol. `supplies: labels` joins the supplies by labels on every pin, VCC above and VEE below unless they are named, with a ground symbol on each ground pin. A DC source's net to a supply pin, or a net named as a supply (`vcc`, `vee`, `vdd`, `v+`, `avdd`, `vbat`), is a rail however few parts are on it. Checked by eye on the exported images of an inverting amplifier: arranged by default, with the feedback below, with labelled supplies, and with the parts kept in place. |
| **2. Library parts by pin name.** | A library part's pins take the names its model gives them. A net `_netINN` or `_netP_INN` names its pin INN, followed through a `.Def` that only wraps another. This happens only when every pin gets a name of its own, and a name the symbol already gives is kept. That covers the TI and Boyle op-amps (INN, INP, OUT, VCC, VEE) and the LEDs (C and A). `add_component` returns the names. `connect` takes `U1.inp`, in any case. A wrong pin's error lists the names ("U1 has no pin in+ (it has 5: INN, INP, OUT, VCC, VEE)"). `replace_component` maps pins by name, and the netlist map shows them. |
| **3. A verified-library flag.** A `find_library_component` field from a nightly run over the libraries. | See *The libraries under ngspice* below. `find_library_component` gives each library part an `ngspice` field: "tested: it netlists and its operating point converges, each pin to ground through 1 MOhm (2026-09-29, ngspice 46) - not a test of what it does", "tested: fails - " and why, or "not tested". `tested: true` lists only the parts that passed. The results are read from `ngspice-tested.json` beside the libraries, and read again when that file changes. |
| **4. Answer size controls.** | `batch` takes `brief`: each call that succeeds is one line (its name, type and place, or its first sentence), and a failure is given in full. `simulate` takes `brief`: no ngspice log, and the long lists (devices, nodes, warnings) cut to the first entries, with "… n more". |
| **5. `tune` on more than one knob; a checked `measure`.** | `tune` takes `knobs` (2 to 4 parts, each with its own range) and `targets` (as many measurements, each with its own `measure`, `target` and `tolerance`). It solves them together by Broyden's method: a Jacobian from one step per knob, then secant updates, each step limited to the knobs' ranges. The values found are set as one undo step, and only when every target is met. The report's kind of case, a chain from 10 V with R1 and R2 for v(a) = 6 V and v(b) = 2 V, gives R1 = R2 = 2.0005k in under a dozen runs. `measure` is checked (bugs above). |
| **6. Before/after built in.** | `simulate` takes `compare`: a dataset kept before (`keep_as` already existed) and the measurements to make. It answers with a table: each measurement in this run, in the kept one, and the change, absolute and in per cent. In the test, halving a lowpass's bandwidth shows as 159 Hz → 80 Hz, −50%. |
| **7. Undo that covers files.** | Every file a call writes is recorded before it is written: its old contents, or that it was not there. That covers `save_document`, `create_subcircuit`'s file, `export_netlist`, `copy_document` and the imported library of `import_netlist`. The records are one step per outermost call (a `batch` is one), and a file written back unchanged is dropped. At most 50 steps are kept. `undo` with `files` (`true`, or a count) takes the last steps back. A file the call made is removed, and one it wrote over is written back. A file changed since, by anyone, is left as it is and named. An open document on a file put back is loaded again. `undo_history` lists the steps as `files written`. A preview writes no step. |
| **8. Small ones.** | `import_netlist` takes `replace`. `redo to` refuses a step behind. A `--workspace` run keeps its scratch there, and the netlist map names the pins (bugs above). Check Schematic warns when a part's supply pins (VCC, VEE, VDD, VSS, V+, V−) are on nothing that powers them: no source and no other part, only supply pins. It adds a note when a DC source has its + on ground: "V3: its + is on ground, so vee is at -15 V - a negative supply is so; if it was to be positive, turn it round …". |

## The libraries under ngspice

The fifth round found all three 741s of `OpAmps.lib` broken under ngspice.
The first two faults were in the translation of library models, and are
fixed in `qucs2spice`:

- **A quoted value is one field.** `C="30 pF"` was split at its space and
  netlisted as `CC1 _net6 _net19 30`, thirty farads. Model parameters are
  normalized one by one now: `R="50 MOhm"` had been 50 Ω and a diode's
  `Cj0="10 fF"` two parameters. 447 values in the shipped libraries are
  written with a space: the varistors' `R`, op-amp, diode and crystal
  models.
- **A library `Idc` flows as a schematic's.** A schematic's `Idc` is
  written with its pins swapped; a library model's was written in
  Qucsator's order, so the TI and Boyle 741s' tail current flowed the
  wrong way. Their output sat at −12.9 V. Five libraries use `Idc` (62
  lines). Controlled sources' values are normalized too.

`theOpAmpLibrarysModelsAreNetlistedRight` checks the transistor-level
741's `CC1 … 30p`, and runs a non-inverting amplifier of gain 11 with the
TI model under ngspice: 1.1 V out for 0.1 V in. It then replaces the TI
part by the Boyle one by pin name, and the output is still 1.1 V.

The third wish asked for a run over every part.
`scripts/ci/test-library-parts.py` drives `qucs-s --mcp-server`. Each
server gets settings, a home and a workspace of its own in a temporary
folder, and `QUCS_LIBRARY_DIR` points it at the source tree's libraries.
Each part is placed alone on a new schematic, each pin labelled and tied
to ground through 1 MΩ (a DC path for every node), and its operating
point run with ngspice. A part passes when it is placed, netlists and
converges. This is a smoke test: it finds a model the translation breaks
or ngspice refuses, not one that runs and does the wrong thing. The
reversed tail current would have passed it, and the 30 F capacitor too.
The results go to `library/ngspice-tested.json`. It is installed with the
libraries and copied into the macOS bundle.
`.github/workflows/library.yml` runs the script each night on Linux with
the distribution's ngspice. The job fails when a part that passes in the
committed file fails now, and keeps the night's file as an artifact.

The run covers 4,224 parts in 62 libraries and takes about five minutes
with six servers at a time. 4,039 parts pass. 77 have no pins: ferrite
core and substrate data that other parts use, recorded as "not tested".
108 fail. Built before this round (the release in the DMG, `8525892`),
the same run passes 1,296. The run found two faults, fixed here, and
the comparison with the old build had one more thing to tell:

- **A library part whose model is one component line was placed as a
  `Lib`.** 2,732 parts: 1,011 in BJT_Extended, 773 in VDMOS, 331 in
  Transistors, 169 in VDMOS_IR, 145 in JFETs, 121 in Diodes, and the
  rest in eight more libraries. Their model is a single `<Diode …>` or
  `<_BJT …>` line with the library's values. The library panel places
  that component (`makeModelString`). `add_component` with type `Lib`
  made a `LibComp`, which copied the component into itself. Its netlist
  then read the diode's first two values as its library and part:
  "Cannot load library component "1.1718" from library/4.2156e-14".
  Under a library's default symbol, it named a subcircuit there was none
  of. `add_component`, `set_schematic`'s components and
  `replace_component` now place that component, as the panel does, and
  say so ("Varactor/BB833 is a Diode with the library's values: placed
  as one"). `edit_component` refuses to make an existing `Lib` into such
  a part and names `replace_component`.
- **Model names that are not the part's.** Nine NMOSFETs' models were
  named after a sibling package: IRFR110's `.Def` was
  `NMOSFETs_IRFRU110`, and IRFB4229PBF's was `…IRFS4229PBF`. One zener's
  was `Z-Diodes_1N5377B`, where the other 93 are `Z_Diodes_`. The two
  all-pass filters' SPICE subcircuits were `Ideal_APF1` and `Ideal_APF2`.
  In each case the netlist named a subcircuit that was not defined.
  These are renamed in `NMOSFETs.lib`, `Z-Diodes.lib` and `Ideal.lib`,
  and all twelve pass.
- **The comparison ran into the fifth round's 2.4.**
  Its six servers shared one scratch folder, so each read the others'
  results, and 29 parts looked worse than before. That old build also
  wrote its scratch schematics into the user's
  `~/Library/Caches/qucs-s/projects`: 14 folders of `untitled*.sch`,
  since removed. In the shared `qucs-s` folder there, `spice4qucs.cir`
  and `log.txt` were overwritten, and those cannot be put back. The
  comparison was run again with the old build's scratch folders set in
  its own settings, and the cache was checked unchanged afterwards. The
  new build keeps every server's scratch in its own workspace. One part
  was then worse: OpAmps' LM3886, whose operating point does not end
  with its rails tied to ground through 1 MΩ. The only difference in its
  netlist is the direction of its seven `Idc` sources, which was this
  round's fix. As a follower on ±30 V, 1 V in, the new netlist gives
  1.010 V out. The old one gives −28.2 V, with internal nodes at 62 MV
  and −1.07 GV: its operating point converged, to nonsense. A part that
  times out is now recorded as "did not end in 30 s with each pin to
  ground through 1 MOhm - a part biased from its supply pins may need
  them powered".

The 108 that fail are pre-existing faults (the old build fails them the
same way), not fixed here:

| Parts | Why |
|---|---|
| 37 digital (Digital_HC 13, Digital_CD 12, Digital_LV 11, Digital_XSPICE 1) | No SPICE model: the netlist names a subcircuit only Qucsator's digital simulation has. |
| 42 TubesExtended | Their behavioural equations call `limit()` (29) and `pwrs()` (8), which ngspice's B source has not; 5 end in "fatal error". |
| 7 (VoltageComparators 5, Optocoupler 2) | `if()` in an equation, which ngspice has not. |
| 3 PWM_Controller | `{{…}}`, which ngspice substitutes only inside `.for`. |
| 19 others | One or two each: a parse error, a model not found inside a subcircuit (`slatch1`), an OpAmp_140 model that runs no analysis, LM3886 (above). |

To learn what the smoke test cannot, OpAmps.lib was also run in a real
circuit: a non-inverting gain of 11 on ±15 V, 0.1 V in. Twelve parts
give 1.09 to 1.14 V: the transistor-level uA741 1.1045 V; ua741(TI)
1.1008 V, which was −12.93 V before this round; ua741(boyle), op27(mod),
op27(boyle), opa27(TI), tl081(TI), tl071(TI), OP07(TI), mc1458(TI),
AD825 and OP37(TI). LM3886 works as the follower above. `ua741(mod)` has
no operating point in this circuit, in the old build or the new, though
it passes the smoke test. The library stays in `ngspice.blacklist`,
which hides it from the library panel under ngspice, as upstream has it.
Thirteen of its fourteen parts work there now, and one does not; whether
to show it is a choice left open.

## Tests

- **`test_qucs_control`:**
  - `aDividerInARowKeepsItsNets`: the three straight-wire variants (bugs above), and a fourth where a ground wire runs through the place the instance's first pin would take where the group was: the instance goes elsewhere, and no pin is left sitting unjoined on another net's wire.
  - `theSixthRoundsSmallThings`: `redo to` behind refused; `measure`'s `waht` refused with "Meant what?"; `import_netlist`'s existing `save_as` refused before anything is made and written over with `replace`; the netlist map's pin names.
  - `theLayoutHelpers`: `near` beside, below and with a gap, its refusals and the nudge off a pin; `connect` round a side and through points, and a `via` that no route passes; `text_at`; `arrange` with `keep_places` (no part moves, every net as it was), `feedback: below` (the feedback resistor under the op-amp, its pin under the output), and `supplies: labels` (VCC above, VEE below, ground symbols).
  - `briefAnswersAndBeforeAndAfter` (ngspice): a brief batch's lines and a failure in full; a brief `simulate` without its log; `compare` against a kept run (bandwidth −50%), and its refusals.
  - `twoKnobsAreTunedTogether` (ngspice): R1 and R2 for two voltages, one undo step, and the refusals (one knob, a mix of operating point and dataset, a misspelt field).
  - `filesAreUndone`: a file made and removed; one written over and put back, its open document reloaded; an exported netlist; a file changed since, left and named; a preview writes no step; `files` with `steps`, and `redo` with `files`, refused; a subcircuit's file taken back.
  - `suppliesAreChecked`: an op-amp with its supplies open (a warning naming both pins), then powered (none); a Vdc with + on ground (the note, with the level); a negative value the other way round.
  - `theOpAmpLibrarysModelsAreNetlistedRight` (ngspice): above.
  - `theLibrarysTestIsReported`: `find_library_component` with no results file (every part "not tested"), with one (passed, failed with its reason, untested), with `tested`, and a new file read again.
  - `aOneLineLibraryPartIsThatComponent`: a varactor placed by `add_component` and by `set_schematic`'s components is a `Diode` with the library's values, and netlists as one; a MOSFET of a library with a default symbol is a `_MOSFET`; `edit_component` refuses to make an LED's `Lib` a varactor, and `replace_component` places it as a `Diode`.
  - Extended: `theFourthRoundsArguments` (the new refusals of `new_project`), `aSavedSymbolIsTakenByItsInstances` (the new advice).
- **`test_mcp_server`:** `theWorkspaceIsTheOneGiven`: the scratch folder (above), and `QUCS_LIBRARY_DIR`: a library in that folder alone is found by `find_library_component`.
- **`scripts/ci/check-tool-arguments.py`:** the new nested arguments (`tune`'s `knobs` and `targets`), and `redo`'s `files` (read by `undo`'s handler). It reports 67 tools checked, 0 disagreements.

Each change was broken on purpose, one at a time: 44 breaks, each built
and its tests run by a script, which put each file back and checked that
it was as before. Five were not caught at first, and the tests were
extended until they were:

- The placement search for the subcircuit instance: in the three
  variants nothing was where the group had been. In the fourth, the
  instance left there has a pin on the ground wire. A file's loader does
  not join a pin to the middle of a wire, so the nets were still right,
  but the pin looked joined. `check_schematic` says so, and the test now
  asserts that it does not.
- Quote grouping in `qucs2spice`: the first break (dropping the removal
  of the space in `"30 pF"`) still gave `30p`. The break of the grouping
  itself is caught.
- The one-line part in `set_schematic` and in `replace_component`: a
  `LibComp` that copied a varactor's diode into itself also saves as
  `<Diode …>` and calls itself a Diode, so both looked right. What they
  get wrong is a library with a default symbol (no copy), and the
  copied object's own netlist. The test now puts in a MOSFET of
  NMOSFETs.lib through `set_schematic`, and checks the netlist after
  `replace_component`.

Three are not caught, and each is said here:

- **The nets check after `create_subcircuit`.** It undoes the change
  when a net differs, but with the other fixes in place nothing reaches
  it. The break of the labels on the ports' nets shows it working: that
  change was refused with "The instance would not keep this schematic's
  nets - SUB1.2 was …".
- **Broyden's update in `tune` with knobs.** Without it, the first
  Jacobian is used throughout (a chord method). On the test's resistor
  chain that takes 7 runs instead of 6. Any bound between the two would
  be fitted to this circuit.
- **A preview's writes left out of `undo files`.** A preview puts its
  file back, so the step is dropped anyway, as a file unchanged by the
  call. Two guards, one of them enough.

The full suite passes (77), under ASan and UBSan too, with no sanitizer
report. `~/QucsWorkspace` is as it was before the runs.

The tools sent every turn grew by 6,531 bytes, from 104,857 to 111,388 as
compact JSON (about 1,600 tokens). Most of that is in `tune` (+1,857:
`knobs`, `targets`, and `measure`'s fields, listed a second time for the
targets so that a misspelt field there is refused too), then `arrange`
and `simulate` (+1,017 each) and `connect` (+549).
