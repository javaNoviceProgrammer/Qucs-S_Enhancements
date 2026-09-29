# Bug hunt: Check Schematic (the electrical rule check)

*29 September 2026 - Qucs-S 26.1.4, `d15ebe2`.*

Aimed at `erc.cpp`, the rule check behind *Simulation → Check Schematic*, the status bar's
problems chip, the check before each simulation and the Claude tool `check_schematic`. It came
after `d15ebe2`, which made the check honour *A schematic must have a ground symbol* (it warned
of a missing ground when told not to). The Release build, macOS arm64, ngspice 46.
Settings were isolated, and `~/QucsWorkspace` and `~/Library/Caches/qucs-s` checked unchanged
after each run. Methods:

1. **The survey of the examples** (`test_erc surveyOfTheExamples`, `QUCS_ERC_SURVEY=1`): every
   shipped schematic, 251 that open, checked and each finding listed. It now checks each example
   for the simulator of its folder, with a ground symbol required and not, and writes every
   finding to a file (`QUCS_ERC_SURVEY_OUT`). A finding on a working example is a false alarm,
   or a fault of the example, or a file Qucs-S does not find.
2. **What the simulators do**, asked of ngspice itself where the check assumes something
   (`case.cir`, `dup.cir` beside this report).
3. **Reading the check against the netlister**: what each rule counts as a part, a net and a
   ground, and what the netlist writes for the same drawing.

*Status:* A1-A4 and B1-B2 fixed in `5d7c0cf`, with tests in `test_erc`; each fix
was broken on purpose and its test failed (11 breaks, each caught). C1 is the survey itself.
The examples of section D are left as they are. The survey went from 6 files with errors and 15
with warnings (every file checked for ngspice) to 4 and 7, and what is left is section D.

| | severity | area | finding |
|---|---|---|---|
| A1 | medium-high | names | `R1` and `r1` stop ngspice ("device already exists"); the check says nothing |
| A2 | medium | net labels | Labels `Out` and `out` are one net for ngspice and Xyce, two for the check |
| A3 | medium | shorted parts | A part switched to shorted joins its pins in the netlist, not in the check: false "floating" and "no DC path" |
| A4 | medium | SPICE library parts | A SPICE library part that cannot be loaded is told only by its symptoms, never its cause |
| B1 | medium | subcircuits | A subcircuit named without `.sch`, as Qucs wrote them, is not found: it has no pins |
| B2 | medium | SPICE libraries | A library named by its path in another installation is not found in this one |
| C1 | low | the survey | Every example was checked for ngspice, without the installed symbols |
| D1-D6 | - | examples | What the check says of six examples, rightly |

## A. The check wrong, or silent

### A1. `R1` and `r1` stop ngspice ("device already exists"); the check says nothing

**Fixed in `5d7c0cf`.** For ngspice, Xyce and SPICE OPUS (an analog circuit), two components of one kind
(`SpiceModel`) whose names differ only in case are an error: "r1: the same name as R1 (at 60, 60)
for Ngspice, which reads names without regard to case". Two parts of different kinds are not:
their SPICE names differ by their first letter (`RR1`, `Cr1`). Qucsator tells the names apart,
and is not told. Test: `namesAreReadWithoutCaseBySpice`.

The check found two components of one name by comparing names as written. A resistor `R1` and
one named `r1` pass, and the netlist has `RR1` and `Rr1`. ngspice reads its input without regard
to case, so the second is the first again:

```
Error on line 4 or its substitute:
  rr1 out 0 1k
device already exists, bail out
```

(`dup.cir`.) The run stops; the check had called the schematic clean.

### A2. Labels `Out` and `out` are one net for ngspice and Xyce, two for the check

**Fixed in `5d7c0cf`.** For the SPICE simulators, labels whose names differ only in case are one net in the
check too, as in the netlist. When the wires and the labels as written keep them apart, a
warning says so: "the labels Out, out are one net for Ngspice, which reads names without regard
to case". Joined by a wire as well, nothing is said. `wiring()` and `notes()` (Claude's tools)
see the same nets. Test: `namesAreReadWithoutCaseBySpice`.

The netlist writes a label's name as it is (`spicecompat::normalize_node_name` changes only
`gnd`), and ngspice joins `OUT` and `out` (`case.cir`: both at 0.5 V). A user who labels one net
`Vin` and another `VIN` gets a short the check did not see. And where the case-joined label is
what holds a part to the circuit, the check called that part floating.

### A3. A part switched to shorted joins its pins in the netlist, not in the check

**Fixed in `5d7c0cf`.** `topologyIssues` joins a shorted part's pins, for what reaches ground and for the DC
paths, and leaves it out of the parts that can float. A ground symbol switched to shorted stays
no ground. Test: `aShortedPartJoinsItsNets`: V1 drives R1, shorted; beyond it R4 and R5, and in
one version C1 to ground.

`Component::getSpiceNetlist` writes a shorted part as a resistor of 1e-12 Ω from its first pin
to each other one (0 Ω for Qucsator). The check left every part that was not active out, so the
circuit beyond a shorted part was cut off. With C1 it said "net R4.2 reaches ground only through
capacitors or current sources (C1): no DC path". Without C1 it said R4 and R5 were floating. Both
are wrong: through R1 they are on V1.

### A4. A SPICE library part that cannot be loaded is told only by its symptoms

**Fixed in `5d7c0cf`.** Three errors for a SPICE library part (`SpLib`), each ending, when the part has no
pins, "it has no pins, and what was wired to them is on nothing":
- "X4: no SPICE library file is given";
- "X5: its SPICE library /nowhere/Missing.lib is not found (beside the schematic, in the project
  or its user_lib, nor in the library of Qucs-S)", told even when a symbol file gave the part its
  pins, since the netlist includes a file the simulator cannot read;
- "X6: its SPICE library .../XyceDigital.lib defines no subcircuit NOSUCH".

Test: `filesNamedAsElsewhereAreFound`.

A library part (`Lib`) and a subcircuit (`Sub`) that could not be loaded were told, with why. A
SPICE library part was not. With the automatic symbol it is then a box without
pins, and the check listed each wire end that met a pin as "connected to nothing". Five Xyce
digital examples showed 41 such findings and not one word of the library (B2).

## B. Files not found that were there

### B1. A subcircuit named without `.sch`, as Qucs wrote them, is not found

**Fixed in `5d7c0cf`.** `Subcircuit::getSubcircuitFile` tries `name.sch` when the name has no suffix and is
not found as it is. The netlist's names are the same either way (`misc::properName` drops `.sch`).
Test: `filesNamedAsElsewhereAreFound`.

`external_interface/probe_and_subcircuit` uses `"example_sub_subcircuit"` and
`"example_subcircuit"` for its subcircuits' files. Qucs looked for `<name>.sch` beside the
schematic (`Subcircuit::getSubcircuitFile` in Qucs 0.0.19); Qucs-S does not, so both subcircuits
had no pins, and the check said so rightly ("its subcircuit example_sub_subcircuit is not
found"), with the loose ends and a floating R1. Now the two files find their subcircuits, and
the check finds nothing wrong with them (for Qucsator, their simulator: C1).

### B2. A library named by its path in another installation is not found in this one

**Fixed in `5d7c0cf`.** `misc::properAbsFileName`: a path with a folder `library` in it, not found elsewhere,
is looked for in this installation's library (`QucsSettings.LibDir`) by what follows that folder.
Backslashes count as slashes, since a Windows path read on macOS or Linux is one name otherwise.
Test: `filesNamedAsElsewhereAreFound`, with `C:/QUCS-S 24.3.0/...`, `C:\QUCS-S 24.3.0\...` and
`/usr/share/...`.

The Xyce digital examples (`testCombLogic1`, `testCombLogic2`, `testNAND2`, `testPATGENX1`,
`testPATGENX4`) name `"C:/QUCS-S 24.3.0/share/qucs-s/library/XyceDigital.lib"`. It was looked for
beside the schematic, in the project, its `user_lib` and the path list, but not in the library
that ships the same file. Every part of those examples was a box without pins, on any machine but
the one they were drawn on. Now they load with their symbols (`library/XyceDigital/*.sym`) and the
check finds nothing wrong.

## C. The survey

### C1. Every example was checked for ngspice, without the installed symbols

**Fixed in `5d7c0cf`** (`surveyOfTheExamples`). The survey checked all 251 examples for ngspice. The
`qucsator/`, `xyce/` and `external_interface/` examples got findings their own simulator would
not give ("ETR1: not available for Ngspice"). A test build has no `share/qucs-s/symbols` beside
`bin/`, so a SPICE library part with a named symbol (`opamp5t`) got the automatic one, and its
wires ended on nothing: the 15 pins, 3 wire ends and 3 grounds of `lorenz.sch`. The survey now checks each
file for its folder's simulator (`external_interface/` for Qucsator), both with the ground
required and not, and gives the test a `share/qucs-s/symbols` linked to `library/symbols`, as an
installation has it.

## D. What the check says of the examples, rightly

- **D1.** `ngspice/Devices/MESFETs` (three files): the library part MESFETCL1 of `MESFETs`,
  which Qucs-S does not ship. The error says so; the loose ends and a ground on nothing follow.
- **D2.** `ngspice/XSPICE_CM/testRFind.sch`: its subcircuit `lspice2g6.sch` is not shipped.
- **D3.** `ngspice/OpenVAF/Tunnel_Ngspice_prj/tunn.sch` does not open (as in every survey).
- **D4.** `Tube_amp6V6.sch`: TRAN1's pins 5 and 6 are connected to nothing. They are `nH` and
  `nB` of `Transformers_LossyTransformer2`, the field and the flux, there to be looked at. The
  check cannot tell a pin to look at from one forgotten; the warning stays.
- **D5.** `Puls3b.sch`: a stub from RI's pin to 520, 80 with nothing at its end (the label is on
  a wire of no length at the pin).
- **D6.** `VCO_100.sch`: L7 sits between C5 and C6, so its nets reach ground only through
  capacitors: "no DC path". For a SPICE simulator that is a singular matrix. The example is for
  Qucsator, which was not run here (not installed).

## E. Checked and found right

- **The ground setting** (`d15ebe2`): no example differs between a ground required and not (none
  lacks one), and the survey now checks each file both ways.
- **The case rules** find nothing in the 251 examples: no example has two labels or two names
  that differ only in case.
- **Inactive parts**: an open part is out of the netlist, and out of the check's nets, pins and
  parts; since `d15ebe2` an inactive ground symbol is no ground for the simulators either.
- **What errs toward silence, not false alarms**, left as it is: a transformer's windings and a
  controlled source's input and output are taken as joined, so a floating secondary or a node
  held only by a control input is not told. Only capacitors and current sources block DC.

## Reproducing

- The survey: `QT_QPA_PLATFORM=offscreen QUCS_ERC_SURVEY=1 QUCS_ERC_SURVEY_OUT=survey.tsv
  build/qucs/tests/test_erc surveyOfTheExamples`. The file has one finding a line: file,
  simulator, ground (`required`/`free`), E or W, message. `survey-before.tsv` is the run at
  `d15ebe2`, every file for ngspice; `survey-after.tsv` the run with the fixes (absolute paths
  replaced by `<source>`).
- `case.cir` and `dup.cir`: `ngspice -b case.cir` prints `v(out)` and `v(OUT)` both 0.5 V, and
  `v(x)` 0.5 V with R4 to `GND`; `dup.cir` stops with "device already exists".
