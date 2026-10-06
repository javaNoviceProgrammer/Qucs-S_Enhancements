# Feature gaps: Claude's wishlist of 2 October

*6 October 2026 — Qucs-S 26.1.5.*

Claude wrote a wishlist (`qucs-s-wishlist-2026-10-02.md`) at `fcbacf0`, in
five sections:

- the Tools menu's synthesis and calculation programs as tools;
- texts drawn over something, noted and moved clear;
- six small things;
- placing a two-pin part without a table of rotations;
- two bigger ideas: several open projects, and the manual offline.

Each item was reproduced on `1b5be7e` through `qucs-s --mcp-server` before
anything was changed. The server ran with its own HOME, settings, trash and
cache, and the source's library. Section 3 went first, as the wishlist
suggests. Each change has a test, and each was broken on purpose to see its
test fail.

## 3. The small things

| # | Wishlist | Now |
|---|---|---|
| a | `compare` against a run kept with `keep_as`: an equation variable (`ac.gain_db`) was "not measured on both runs (value)", though `get_dataset` measured it on that run's file. | Reproduced as written: the kept run's bandwidth said "the curve goes below 0: it is not a magnitude, and not known to be in dB". The cause: a variable's measuring options were made for this run's curve only. Those are its unit (an equation's `dB(...)`), its phase for the margins, and an eye's PRBS source. The other run's curve was measured with bare options. **Now** both runs' curves are measured with the same options; the eye's source is read from the netlist the kept run had. Before and after are both filled: 1588 Hz and 794 Hz. When a side has no number, the note says which run and why, or that the field is none the measurement gives ("gain gives no 'dc gain dB': 'field' is one of at the first x, note, peak, unit, value"). |
| b | Eye diagram drawn as a density: `edit_trace {thickness: 3}` was taken and changed nothing. | Reproduced. Density counted a trace a pixel wide, whatever its thickness. So did the "many traces" mode above 2000 windows, which is counted the same way. **Now** thickness 1 (or 0) is a pixel, as before. Above 1, the trace is counted as wide as a line of it is drawn: a disc of that many pixels, each pixel counted once a window. |
| c | A noiseless PRBS showed Q = 3×10¹⁵. | Reproduced differently, and worse. A 0–1 V ideal source gave no Q at all (no spread). A 3.3 V one gave **Q = 4.1×10¹⁴**: rounding, since 3.3 is 3.3000000000000003 at some samples. **Now** a spread below a billionth of the levels' spacing is no noise, and Q is infinite. Beside the diagram it reads "Q ∞ (no noise)". In JSON (`get_dataset`'s eye, `add_diagram`'s measured) it is `"Q": null` with `"Q note": "infinite: no noise - each level is the same at every bit's centre"`. A millivolt of noise gives a Q as before. |
| d | Projects panel menu: on a project that isn't open, Close Project was offered, and would close the open one. | Reproduced. **Now** it is greyed on another project's row, as Open Project is on the open one's. It stays on the open project's row and on no row. `test_workspace_projects` had asserted the old behaviour: Close Project chosen on another row closed the open one. It now asserts the new rule. |
| e | `get_settings` had no filter: `scope: app` was about 8,000 characters (67 settings) every time. | Reproduced: 8,256 characters, 70 settings. **Now** `keys` takes a key, a wildcard (`Locations/*`) or a label alone. `search` takes words, each to be in the key, the value or the choices. Without `scope`, both look through app, simulators and cdl, and each setting says its scope. `search: ngspice` gives 6 of 87. **Found on the way**: the tool's own description gave `"Locations/Ngspice"` and `"Settings/Language"` as keys, and neither exists. They are `Simulators/Ngspice executable location` and `Settings/Language (set after reload)`. `set_settings`' error gave the second as its example too. Both now name real keys. |
| f | A trace not from a PRBS source, with few transitions (PRBS31's slow start: 34 crossings in 300 bits), was read as 3 UI. | Reproduced: PRBS31's first 300 bits, imported as a table so no source tells the UI, gave 310.6 ps, 3.1 UI. The first guess, a tenth of the way up the sorted gaps, was 3 UI. Few gaps were whole multiples of it, and the scan that follows settled at 3.1 UI. **Now** the shortest gap is a second guess. It is taken when more gaps are whole multiples of it, at least 80 %; half a UI divides them all too, so of as many, the longer guess wins. The result is 99.9998 ps. The readout and the JSON note say when the estimate is doubtful: fewer than 50 crossings ("the unit interval is told from only 34 crossings: give it if that is not a bit's length"), fewer than 95 % of the gaps a whole number of it, or a gap shorter than it. |

## 2. Texts drawn over something

| Wishlist | Now |
|---|---|
| `check_schematic` notes for a text over a wire, a symbol, another part's text, a label or a diagram, with both boxes. | Each part's text and each net label's text drawn over something is a **note**, with `box` and `over`. The obstacles are a wire, a symbol, another part's text, a net label, a diagram, or a painting of text. Two diagrams drawn into each other are a note too. A symbol is taken as it is drawn: its lines, arcs and outlines, its filled shapes, its own texts and its pins' ends, not its bounding box. A text is taken by its letters (`tightBoundingRect`), not its line's box. These are the new `textplacement.cpp`. A census of the 247 example schematics that run no commands checked both choices. Boxes gave 243 notes, 101 of them a text over its own symbol (an op-amp's name in its triangle's empty corner in `notch.sch`). Strokes gave 112, and letters 90. The ones looked at are real: `R1`'s "Letter=R" over the resistor (`Xyce2ToneTest.sch`), "alpha=90" over the radial stubs (`LPF_1000_Radial-Stub.sch`), net labels crossed by wires (`testCombLogic1.sch`). |
| `edit_component {text_at: "auto"}`, or `arrange {labels: true}`: texts moved to the nearest free spot beside each part, never a part or wire, in a fixed order of sides (right, left, below, above). | **`text_at: "auto"`**: the nearest spot beside the part where its text is drawn over nothing. The sides go right, left, below, above; a grid step further out, or along a side, costs more. A text that is clear stays, and the answer says so. The text's old place is no obstacle to itself. **`arrange` with `labels`**: only texts move, each part's and each net label's that is drawn over something; the others stay. A label tries the side away from the part whose pin it names first. On a 30-part board imported by `import_netlist`, overlaps went from 18 to 0. The netlist, every part's place and every wire stayed as they were, in one undo step. With another option it is refused. `edit_component` also says when a part moved, turned or with its texts changed now has its text over something, naming what, with the hint. |
| `add_component` places a new part's text in a free spot, and says when it can't. | Its type's place is kept when it is clear. Otherwise the text goes to the nearest free spot, and `text` says where; if none is free, it says what the text overlaps. 1 October's Cvcc beside VCC, at its type's `[-70, -26]`, was over VCC's symbol. It now goes to the right of it, and no other text moves. A `text_at` given is kept, and noted if it overlaps. |
| Diagrams: a title running into the x-axis label of the diagram above; a gap by default when one is placed below another. | `add_diagram` with no position already leaves 80 units: two diagrams stacked so are clear of each other, and the test checks it. But `add_diagram`'s and `edit_diagram`'s warning compared frames only. It now compares what each draws: "Drawn into another: its title runs into diagram 1's x-axis label - y 1258 or more clears it". `check_schematic` notes it too. **Found on the way**: "Placed below the circuit, its lower left corner at 160, 1009" gave y before it was put on the grid (1010). It now gives y after. |

## 1. The synthesis tools

The programs are executables of their own. Their sources clash with each
other's and with Qucs-S's (two `Filter` classes, two `qf_poly.h`), so linking
them in was not the way. The wishlist's fallback was: each program takes
**`--json`**, reads a spec on standard input, and writes its result on
standard output. No window is shown and no setting is read or written. The
two whose calculation lives in their window's fields (Power combining, Line
calculation) fill the window in on the offscreen platform and press its
button. `qucs/tooljson.h` is the shared input and output, header only. Qucs-S
finds a program where its Tools menu does (`BinDir`, an app's
`Contents/MacOS/bin`), in `QUCS_TOOLS_DIR`, or in the build tree it was built
in. Matching Circuit is Qucs-S's own dialog, and it runs in-process.

A design is placed as its paste places it. It goes into a new schematic,
saved as `save_as`, or into the schematic `path` names at x, y; when x, y are
not given, it goes below what is there. `Schematic::pasteText` is the text
part of a paste, without the system clipboard's image. Names are numbered as
a paste numbers them, the design is one step to undo, and the answer says
when its ports are numbered on from ports already there. A design with a
value that is no number (`nan`, `inf`) is refused.

| Tool | Now |
|---|---|
| `synthesize_filter` | **LC**: `qucs-sfilter`'s calculation is split from its window (`QucsFilter::schematicOf`), with every realization, Cauer included. **Active**: `qucs-sactivefilter`'s Sallen-Key, multiple-feedback and Cauer sections. An LC Butterworth or Chebyshev low- or high-pass takes its order from `atten` at `fs`. That uses the program's own convention: a Chebyshev's corner is its −3 dB point. The wishlist's test, a 5th-order Chebyshev, 1 GHz, 0.1 dB, 50 Ω, into a new schematic: 4.142 pF, 12.38 nH, 7.134 pF. Simulated under ngspice, it is **−3 dB at 999.8 MHz, ripple 0.1001 dB**. An active 5th-order Butterworth Sallen-Key is −3 dB at 999.98 Hz. **Found**: the active filter's unity-gain Sallen-Key low-pass had R1 and R2 = nan in each second-order stage. Under the square root was a difference that is 0 by construction and below it by rounding, so the window failed too. It is clamped at 0. |
| `synthesize_attenuator` | `QUCS_Att` as it is. A 10 dB pi: 96.2, 71.2, 96.2 Ω; S21 −10.005 dB. A pi or tee between unequal impedances is refused below its least attenuation, with that value. **Found**: its equations were always Qucsator's `Eqn` with `S[2,1]`. Under ngspice that is a `.PARAM` ngspice refuses, so its S-parameter schematic did not simulate. It now writes them for the simulator, a `NutmegEq` with `S_2_1` for ngspice, as the LC filter does. The attenuator's window reads Qucs-S's simulator setting for this. |
| `synthesize_matching` | Tools > Matching Circuit's calculation, run quietly (`MatchDialog::designOnePort`, `designTwoPort`). No message box is shown, the clipboard is not touched, and what it would have said is in the answer. A load is given as `z_load` ("10-j20"). A two-port is given as its S-parameters, and is refused with K and \|Δ\| when it is not unconditionally stable. **Three faults found**, which Tools > Matching Circuit has in the window too. **(1)** Its equations were Qucsator's whatever the simulator; they are now each simulator's. **(2)** For 10 − j20 Ω to 50 Ω, the L-section's series element has no reactance (R + X²/R − Z0 = 0). It was written as `L2 0.000H`, which ngspice refuses; an element of no reactance is now left out. **(3)** A pure reactance was tested with `RL == 0`, but rounding made it 1e-15, and the quarter-wave transformer came out with nan lines. It is now tested against a billionth of Z0. The wishlist's test, an L-match from 50 Ω to 10 − j20 Ω at 900 MHz, simulates to **S11 −48.5 dB** there. |
| `synthesize_power_combiner` | The window is filled in and Generate pressed. `deliver()` keeps the schematic in place of the clipboard. Under a SPICE simulator, the window offers lumped elements only, and only for the Wilkinsons; anything else is refused with that reason. With `simulator: qucsator` it offers ideal lines and microstrip. A Wilkinson at 1 GHz: S21 −3.010 dB. |
| `line_calc` | Line calculation's window is filled in, in metres, hertz, ohms and degrees, and Analyze or Synthesize is pressed. A small public API was added: `propertyNames`, `unitsOf`, `solveFor`, `analyze`, `synthesize`, `results`. The wishlist's test, 50 Ω microstrip on FR4 (εr 4.4, h 1.6 mm, 35 µm) at 2.4 GHz: **W = 3.028 mm**. It is the window's own calculation, so it matches the window; analyzed back, it gives 50 Ω within 0.5 %. |
| `receiver_budget` | RxCalc's `System` and `Stage`. An IP3 or P1dB not given is +100 dBm, no limit, and is said. The cascade's noise figure is Friis's, checked to 0.01 dB. **Found by the ASan run**: `Stage`'s constructor read its IP3 and P1dB priorities before they were set; UBSan saw "a value no `priority` is". The members now have default values. |

**Found when checking the DMG**: the packaged app, opened from the
repository's `bin/macos/apple-silicon`, found none of the programs: "No
attenuator: qucs-sattenuator ... is not in this build" (looked for in
`Contents/MacOS`). They are in `Contents/MacOS/bin`. `main()` took the
programs' folder (`BinDir`) to be the application's own folder whenever its
path held "bin" anywhere. That included a folder of that name above the
bundle, or a user called robin. The same fault kept Tools' own menu from
starting them from such a place. `db1a289` had fixed it for the resources'
folder, not for this one. It is now `misc::binDirOf`: the application's own
folder only when that folder is `bin` (a prefix install). From the same DMG
copy, `synthesize_attenuator` then placed its pi attenuator. The programs'
own `main()`s cut their path at the first `/bin` the same way, but only for
their translations' folder, which `--json` does not use; that is left as it
was.

Trying the other tools from the DMG found two more.

- **An active filter's `order`.** `{kind: active, response: butterworth,
  order: 4, fc: 1 kHz}` was refused: "'fs' ... gives the order". With `fs`
  given as well, the order given was passed over without a word. The
  window finds a Butterworth's or a Chebyshev's order from `atten` at its
  stop band, but their poles are the order's alone: the stop band only
  picks the order. **Now** an order given is made. The program finds,
  by halving, a stop band edge (`fs`, or `transition` for a band) that
  gives it, with `atten` and `ap` set aside. With a stop band given too,
  the order wins, and a note says so. An inverse Chebyshev's or a Cauer's
  zeros are where its stop band is, so an order given there is noted as
  not used. A band-stop's order must be even, and an odd one is refused
  saying so. A band Bessel or Legendre, whose order is always given, no
  longer needs `transition`. The 4th-order Butterworth simulates to −3 dB
  at 999.98 Hz and −80.0 dB at 10 kHz, two op-amps. **Found by UBSan on
  the way**: the window's order formulas converted their value to an
  `int` however large. The search's nearest stop band, and a stop band at
  the corner (`fs` = `fc`), gave 2.3×10¹², which an `int` cannot hold. Past
  50, the most it makes, is now 51, which it refuses: "cannot be made".
- **`get_dataset`'s `at` at a sweep's end.** At 10000, the `to` the answer
  gives, it was null. ngspice writes the end of a 10 kHz sweep as
  9999.999999999889, so 10000 was just outside it. **Now** a sample
  within 10⁻¹² of the x asked for is at it (`ds::valueAt`). This holds for
  markers and measurements too.

## 4. Placing parts without a rotation table

`add_component` and `edit_component` take **`pin1`** (`top`, `bottom`,
`left`, `right`) instead of `rotation`, for a part of two pins. The turn that
puts pin 1 there is found by turning the part. `edit_component`'s turn keeps
the nets, as a rotation's does. It is refused with three pins or more, an
unknown side, or `rotation` given too. The wishlist's test: C1 placed below
R1.2's node with `pin1: top`, `connect` to the node, and `connect` to ground.
That gives one straight wire up, and the ground on its pin 2 itself. The
other proposal, `connect` turning a part as it wires it, was not done: it is
the alternative to `pin1`, and would change what `connect` does to parts
placed already.

## 5. The bigger ideas

**5b, the manual offline**: `read_help` with `manual` fetches the manual's
text once into Qucs-S's cache (`manual/`) and searches it by section from then
on, offline. `page` gives a page whole, and every later `read_help` topic
searches it too. Sphinx's `searchindex.js` names each page's source
(`index.rst`, `overview/x.md`), read as `_sources/<file>.txt`: the text,
Markdown or reST. `curl` fetches it: Qt Network is not linked into Qucs-S, and
adding it would add its TLS plugins to the app bundle. The real manual is 30
pages, 308 KB, fetched in 3.6 s. `QUCS_MANUAL_URL` names another copy (the
tests' `file://` one). **Found on the way**: in one curl run, after a few
pages that were not there (`.rst.txt` where the page is Markdown), each
following page waited out its 60 s. Only pages the search index names are
fetched now, with 15 s to connect. A first version took the index's
`toctree` for the page list, found Markdown pages under `.rst` names, and
stamped the cache as fetched with the index alone. That was fixed before the
tests.

**5a, several open projects**: not done as a whole. The wishlist conditions
it on a real need ("Worth it only if two projects at once is a real need").
It changes what "the project" means for relative paths, Scratch, Verilog-A
compilation, autosave and the tools. What is done: `get_state` lists the
workspace's `projects`, the open one marked. Each document carries the
`project` its file is in, and `project not open` says when that is not the
open one ("alpha is open, not beta: relative paths, Scratch and Verilog-A are
alpha's").

## Tests

- **`test_wishlist_tools`** (new, 20 functions). Section 3:
  - `aKeptRunsEquationIsMeasured`;
  - `aKeptRunsEyeIsFoldedAtItsSource`;
  - `settingsAreFoundByKeyAndWord`.

  Section 2:
  - `textsOverSomethingAreNoted`: a wire, symbols as drawn, letters not the line's box, two texts once, a label;
  - `autoTextGoesClear`;
  - `aNewPartsTextGoesClear`;
  - `arrangeLabelsMovesOnlyTexts`;
  - `diagramsBelowEachOtherKeepClear`.

  Section 4: `pin1PlacesATwoPinPart`. Section 1:
  - `aFilterIsSynthesized`;
  - `anActiveFilterIsSynthesized`: from `atten` at `fs`, and from an `order` alone (simulated: −3 dB at fc, −80 dB a decade above); an order with a stop band, a Cauer's, an odd band-stop's, a stop band at the corner;
  - `anAttenuatorIsSynthesized`;
  - `aMatchIsSynthesized`;
  - `aCombinerIsSynthesized`;
  - `aLineIsCalculated`;
  - `aReceiverBudgetIsCalculated`;
  - `theProgramsAreFoundWhereverTheAppIs`: a bundle under a `bin`, a user called robin, a prefix install;
  - `aDesignGoesIntoAnOpenSchematic`.

  Section 5:
  - `theManualIsFetchedAndSearched`, from a `file://` copy;
  - `theStateListsTheProjects`.

  The tests that simulate skip when there is no ngspice. The programs are
  built before the test; the top `CMakeLists.txt` adds that dependency once
  their targets exist.
- **`test_prbs_eye`**: `fewCrossingsAreSaid`, `aNoiselessEyeHasNoQ` and
  `aThickTraceIsDrawnThick`. The last includes a single pulse: where its two
  edges cross, a thick trace is no more opaque than two windows'.
- **`test_workspace_projects`**: `theMenu` (Close Project).
- **`test_dataset`**: `curvesAreSampledAndSummed` reads a sweep's end written with rounding.
- **`test_qucs_control`**: `eachArgumentAloneChangesThePart` gives `pin1` a
  value.
- **`scripts/ci/check-tool-arguments.py`**: `receiver_budget`'s stages are
  read by `stageSpec`.

**86 breaks, 85 caught.** The one not caught is taking the texts that will
move out of `arrange`'s obstacles. Without it, one text that moves can avoid
another's old place, which only matters when two moving texts compete for
the same spot. Breaks not caught at first, and what came of them:

- **The kept run's measuring options**: two layers each did the job alone.
  The second, which gave the kept run this run's unit, is gone. A test of an
  eye against a kept run, its bit length from that run's PRBS source, holds
  the first.
- **A pixel counted once a window**: the single-pulse test holds it.
- **What the tests did not yet make**:
  - the letters against the line's box (a wire in the room after a name's
    last letter);
  - its own text no obstacle (a text a little over its own pin);
  - a part's text in `arrange`;
  - the design moved to x, y;
  - a node label put down;
  - the guard against a value that is no number (1e-300 dB: the program's
    resistors infinite);
  - the matching message said in the answer, not in a box.

  Each now has a test.
- **A label's stem through its part**: the check is gone. Once a label tries
  the side away from its part first, no stem can cross the symbol.
- **A receiver stage's unknown field**: the schema's check already refuses
  it. The second check is gone.
- **The four program breaks**: the test target did not rebuild the programs.
  The dependency had been added before the programs' targets existed, and is
  now in the top `CMakeLists.txt`.

The full suite passes, 89 of 89; under ASan, 89 of 89 with no report, four
tests at a time. One ASan run at six at a time had 87 of 89. The two that
failed are outside this work: `test_status_bar` waits 5 s for a fake
ngspice's version, and `test_claude_git` reads a detached head's status.
Both passed on their own. The scenario script passes 97 of 97. The
user's `~/QucsWorkspace` and `~/Library/Caches/qucs-s` are as they were
before the final runs, checked against a marker file.

## Not done

- **5a**: several projects open at once, as above.
- **Section 4's other proposal**: `connect` turning a part. `pin1` is done
  instead.
- **3b's alternative**: `edit_trace` saying thickness does not apply.
  Thickness applies now, so it has nothing to say.
- **5b's first option**: shipping the manual inside the app. The second, a
  fetch on first use, is done; the packaging stays offline.
- **5b's Getting Started tutorial**: it is a PDF online, not in the manual,
  and `read_help` names it as before.
