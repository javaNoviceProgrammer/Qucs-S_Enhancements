# RF 2.5D and HFSS™ 3D Layout: export, import, comparing S

The request: "Can we add export and import to RF 2.5D so that I can export as a 'HFSS 3D layout' project or directly
import a 'HFSS 3D layout' project into RF 2.5D? My goal is to benchmark against HFSS 3D layout at some point."

Ansys® and HFSS™ are trademarks; the Trademarks section of the README says whose.

## 1. Can we? The assessment, before anything was built

**Nothing from Ansys is on this Mac** (searched /Applications, Spotlight and the home folder for AEDT, `.aedt`, `.aedb`,
`edb.def`, PyAEDT, PyEDB: only some old files of another format). AEDT does not run on macOS, and the user has no AEDT to
hand for now. **So nothing written here has been run in AEDT.** What it was checked against is said in section 5.

### What a 3D Layout project is

Read from the program's documentation and four small public samples
(kept out of this repository):

| File | What it is | Readable without Ansys? | Writable without Ansys? |
|---|---|---|---|
| `<project>.aedt` | Text, `$begin`/`$end` blocks. A 3D Layout design is a `PlanarEMCircuit` block: `Layout` > `HfssExportInfo` (the extents), `Port` > `Data` (each port's properties, `HFSS Type`, `Deembed Distance`, `Renormalize Impedence`...), `Setup` > `Data`; the project's `Definitions` > `Materials`. **No geometry and no stackup.** | Yes | In form; useless without the `.aedb` |
| `<project>.aedb/edb.def` | **Not wholly binary, as was expected.** A header (`Version='10.6'`, `Encrypted=false`), a table of 47 record classes and their fields' types, then text blocks (materials, padstacks, variables, one `SLayer(...)` line for each layer of the stack, `HfssExtent`) and binary records for the nets, the shapes and the vias, their coordinates SI doubles with the expression beside each. | The stack, materials and extents: yes, they are text. The shapes: only by decoding records nothing documents (section 3). | **No.** Nothing documents it, nothing here could test it, and a file AEDT misreads would be worse than none |

Nothing public documents `edb.def`. PyAEDT and PyEDB, the open-source libraries, reach it through the libraries of an
AEDT installation rather than reading it themselves.

**So "export as a project" cannot mean writing an `.aedt` and `.aedb` here.** It means writing what AEDT itself turns into
one. And "import a project" can mean reading most of it directly, with the shapes the hard part.

### Export: the routes, and the choice

| Route | Stack | Geometry | Ports | Setup, sweep | Documented | Verdict |
|---|---|---|---|---|---|---|
| **AEDT script (`.py`, Tools > Run Script)** | `AddStackupLayer`, `AddMaterial` | `CreatePolygon`, `CreateVia` | edge, wave, coplanar references (`AddRefPort`), circuit | `Add`, `AddSweep` | Its scripting documentation; one correction found (below) | **Yes: the first route.** The only one that makes an edge port or a coplanar port |
| **GDSII + XML control file (File > Import > GDSII, or `ImportGDSII`)** | `<Stackup>` | the GDS | `<CircuitPortPt>` only: "limited to point-based Circuit port creation" | `<SimulationSetups>` | Its documentation, with a schema | **Yes: the second route, written beside the script.** One documented call, no script needed; its ports are all circuit ports |
| IPC-2581, ODB++ | yes | yes | none | none | formats of their own, large, PCB-minded | No: adds nothing the two above lack |
| Writing `.aedt` + `.aedb` | | | | | no | No: above |

The test is what gives a faithful benchmark: **the same stack, geometry, ports, frequencies and de-embedding.** The script
carries all five where the two solvers have the same thing, and says where they do not (section 4). The GDS + XML route
is there because neither can be tried here: if the script stumbles in AEDT, the same stack and shapes still get in by the
documented translator, as a public Ansys example does for an on-chip inductor.

The documentation's own text was checked against PyAEDT (MIT), which drives AEDT with these commands every day. It found one
error in it: `CreatePolygon` takes `"polyGeometry:="`, not the `"lineGeometry:="` it prints. Likewise `Add`
there shows only a near-field setup; the HFSS setup's property names come from PyAEDT's template, and they agree
with the blocks in a real `.aedt`.

### Import: the routes, and the choice

| Route | What it reads | Needs AEDT? | Verdict |
|---|---|---|---|
| **Our own exported script, by its `#qucs-s` line** | The model whole, as the Lumerical® `.lsf` does | No | **Yes.** Export then import gives back the same model |
| **`.aedt` with its `.aedb`** | Stack, materials and extents from `edb.def`'s text; ports, setups and sweeps from the `.aedt`; shapes from the binary records | No | **Yes, this is the direct import.** The shapes as far as section 3 says |
| **GDSII + XML control file** | What AEDT's own Export Control File and File > Export > GDSII write | No | **Yes.** A documented, open way out of AEDT, and the fallback where the shapes cannot be decoded |
| **A script run inside AEDT writing a neutral file** | Everything, by AEDT's own API | Yes, and cannot be tried here | Written last, if at all: it can only be tested against a stand-in. **In the end not written:** the direct read reached the end of all eleven samples (section 3) |

## 2. What was built

**Export.** RF 2.5D > Export HFSS™ 3D Layout…, or `rf25_hfss_export`. It writes three files beside each other:

- **`<name>.py`, an AEDT script** of data lists and a fixed engine, for Python 2.7 (IronPython) and 3. Tools > Run Script, or
  `ansysedt -RunScriptAndExit <name>.py`. It builds the design whole, saves the project beside itself as `<design>.aedt`
  and leaves `<design>_aedt_report.txt` saying what went wrong, if anything. With the script argument `solve`
  (`-ScriptArgs solve`) it also solves and writes `<design>_hfss.sNp`.
- **`<name>.gds` and `<name>.xml`**, the documented second way in (File > Import > GDSII). Its ports are all circuit ports,
  as documented for that route.
- **The model itself**, in the script's `#qucs-s` comment line, with its shapes, which is what makes the round trip exact.

The export refuses a script whose `.gds` would be the model's own layout, and a name whose `.gds` or `.xml` is there with no
script beside it (so no earlier export of it). Run again, it writes over its own three files.

**Import.** RF 2.5D > Import HFSS™ 3D Layout…, or `rf25_hfss_import`. No Ansys software is needed for any of these. It makes
a `.qrf25` (and a `.gds` of its name where the shapes are in no file already) and opens it:

| Given | Read as |
|---|---|
| An exported `.py` | The same model. Its layout file where it still is, else the `.gds` beside the script, else the shapes in the line |
| An `.xml` control file and its `.gds` | Stack, materials, layer map, circuit ports (as internal ports), sweep. A lowest layer under other metals, one whole shape of it covering the design, becomes the ground plane |
| An `.aedt`, its `.aedb` folder or `edb.def` | A project made anywhere: stack, materials, shapes with voids and arcs, vias, edge and point ports, sweep. Sections 3 and 4 say how far |

It writes over nothing unless told (`replace`, or Yes in the menu's question).

**Compare.** RF 2.5D > Compare With Touchstone…, or `rf25_compare`. It sets the model's full-wave solve beside any Touchstone
file: each port into the reference the file says (recomputed from the solve's Z), read between the file's points in
magnitude and angle, `order` where the file's ports are in another order. For each term it gives how far apart (|S - S'|, and
where), the rms, and the most in dB and degrees; it can write both as a CSV. The dB figure leaves out terms below -60 dB, and
the degrees those below -40 dB, where the numbers say little.

**A benchmark, then:**

1. Export the model. Take the three files to a machine with AEDT.
2. `ansysedt -RunScriptAndExit <name>.py -ScriptArgs solve`; read `<design>_aedt_report.txt` first.
3. Bring `<design>_hfss.sNp` back and press Compare With Touchstone on the solved model.

Or, for a project somebody made in AEDT: import the project, solve it here, and compare with the Touchstone file AEDT exports.

**The first case, dry.** A scratch copy of `bench_ponchak_fgc/sims/fgc_S62_L1000_r15p6_c10.qrf25` (the coplanar line on
polyimide on silicon), through the built app over MCP:

- The export gave three polygons, the stack Si / PI / M1 with M1's fill the air, `P1` and `P2` as gap ports each referenced to
  the 2 edges beside it, `LINC 1GHz 40GHz 40`, dielectrics 2607.5 µm past the shapes and the boundary 1873.7 µm past that.
- Python's parser took the script; re-importing it gave the same model.
- The whole run took 160 s. Comparing the solve with its own Touchstone file gave at most 5.7e-13 apart (the file's rounding).
- Comparing it with the `.s2p` kept in that folder on 2 November gave S11 at most 0.060 apart and S21 at most 0.0086 apart, or
  0.007 dB and 0.51 degrees, both worst at 40 GHz. That is the comparison doing its job on real numbers. The solver has changed
  since that file was written, and nothing here looked into why those two differ.

**A project made in AEDT, whole.** A public sample (a differential via through a 12-metal board, 23 layers, 4 wave
ports on two signal layers), imported directly from `edb.def` and solved:

- It comes in as 59 polygons, 44 via segments, 18 anti-pads cut from the GND and VCC planes, and each port returning through a
  terminal on the plane it names. No model problem is reported.
- At the default mesh it was still solving its one frequency when the tool's 300 s wait ended. That run was closed then, so
  nothing is known yet of how long it takes.
- With a far coarser mesh for this check alone (largest cell 1 mm, 1 cell across, the dense matrix), 1 GHz solves in 5 s, passive and
  reciprocal, S31 = -0.10 dB at -21°, S11 about -20 dB and S21 about -34 dB. **That mesh is far too coarse to mean anything as a
  number.** What it shows is that the import leaves a model that solves, with its ports on the right lines and the
  signal off the planes.
- Exported again it has the 18 voids and separate nets for GND, each signal and each VCC plane.

Five defects came only from this real project, and a sixth from driving the packaged app; a test now holds each (section 5).

## 3. The shapes in `edb.def`

Nothing public documents `edb.def`. This was found from public samples of versions 8.6 to 12.1, and it is kept in
`rf25_edb.h`, `rf25_edb.cpp` and in `test_rf25_hfss.cpp`, whose writer makes small files of the same grammar (no Ansys
file is kept in this repository).

| Value type | Written as |
|---|---|
| 0 | A byte |
| 1 | An int32 |
| 2 | A double |
| 3 | A double and the string expression it is the value of |
| 4 | A string (int32 length, bytes) |
| 5 | A record: its class, -1 for none. A class's first field of type 5 is what it derives from |
| 6 | A list: int32 size, int32 type of its items, the items |
| 7 | An array: int32 size, then each item its type and value |

The file: a byte and a `Hdr` text block (`Version`, `Encrypted`); the table of classes, by id, field count and each field's type;
then text blocks each after its length (the project's materials, padstacks and variables; each cell's name, variables, setups;
its stack, one `SLayer(...)` a layer; each via's use of a padstack, `def=, fl=, tl=`), and after a cell's text its lists of
records.

| Class | What it is |
|---|---|
| 7, 8 | A net; a net class |
| 10 | A primitive's base: its net, parent, kind |
| 11 | On a layer: its layer's id, and at index 4 the shape it is a void of (-1 none) |
| 12 | A rectangle: 2 corners, or (kind 1) centre and sizes; corner radius, rotation |
| 13 | A circle: x, y, r |
| 14 | A path: caps, corner style, width, miter, its line (class 36) |
| 15 | A polygon: its outline (class 36) |
| 36 | Points: doubles or values. An arc is a pair (h, y above 1e100), bowing to the right of the way from p to q for h above 0 |
| 19 | A via: its use's id, x, y, rotation, and a hole size that can differ from its padstack's with nothing set to say it counts (one sample has 1 mm beside a hole of 400 µm in a clearance of 770 µm), so the padstack's is taken. The padstack's text gives each layer a pad and an anti-pad (`lgm lay=... pad(shp=...) ant(shp=...)`) |
| 29 | A terminal: name, reference layer, properties (`HFSS('HFSS Type'=...)`, `PlanarEM(PortSolver=...)`), reference, z0, `PortExcitationProps(...)` |
| 30, 39 | An edge terminal, and its edges (id, x, y, primitive, edge number) |
| 31, 32 | A via terminal, a point terminal |

The reader goes by the file's own table, so a version that adds a field to a class reads the same. It is strict: a class the
table lacks, a length past the end, or a list that does not end where the next begins stops it, and what stopped it is said.
**A shape is never guessed at.** The stack, which comes first, is still handed on, with a note to export a GDSII file instead. An
encrypted file is refused by name.

What the arc's sign was checked against: a sample's bends, which come out smooth and on the line (`all.png`, not
kept). An arc is 16 chords, a circle 64 sides; a path is its pieces united, with round or flat caps as its fields say.

### The samples

| Sample | Version | Holds |
|---|---|---|
| 1 | 9.0 | 1 path, 4 polygons, 1 void |
| 2 | 10.6 | 2 paths, 2 rectangles, 1 void, 38 vias |
| 3 | 12.1 | 23 layers, 4 paths, 7 rectangles, 4 vias, 4 edge ports, a sweep of 200 points |
| 4 | 8.6 | 3 paths, 5 polygons, 2 rectangles |
| 5 | 10.6 | 3 rectangles |
| 6 | 10.6 | Nothing, and no layers: an empty design |
| 7 | 9.4 | 1 path, 4 rectangles, a sweep of 100 points |
| 8 | 10.6 | 3 circles, 8 paths, 8 polygons, 5 voids, 46 vias |
| 9 | 9.0 | 7821 paths, 639 polygons, 4 vias, 1 port, a sweep of 51 points; an Overlap stack |
| 10 | 10.6 | 16 paths, 4 polygons, 165 vias |
| 11 | 10.6 | 7 polygons, 6 voids, 12 vias |

`thePublicSamplesReadToTheirEnds` reads them all where `QUCS_EDB_SAMPLES` names a folder of them (they are not in the repository,
so it is skipped on CI). Each reads to its end and makes a model.

**What the samples could not show.** Most have no ports with an `.aedt` of their own, and only a few have setups. Records of the
kinds nobody uses here (a component, a group's frame, text) are counted and said, not read. A file of a later AEDT version may add
classes; the reader then stops and says so, rather than reading on.

## 4. The mapping, and what a benchmark must know

### Export

| RF 2.5D | HFSS 3D Layout |
|---|---|
| Lengths | µm, as the layout's own unit |
| Substrate, dielectric | Dielectric layer of its own material, a substrate's conductivity kept |
| Metal | Signal layer at its height, with its thickness; its fill the first dielectric above it short of another metal, else `rf25_air` |
| Solved as a sheet | Its thickness in the field here. Option `thin_metal` sets the setup's `ZeroMetalLayerThickness`, like with like |
| Ground: open | No plane |
| Ground: perfect | Signal layer of conductivity 1e30, no thickness, a rectangle past the shapes by the margin, `TruncAtGnd` |
| Ground: metal | The same, 5 skin depths thick at the bottom of the sweep |
| Ground: impedance | The same, a 1 µm sheet of the conductivity that gives it |
| Cover: perfect | Air and a lid. RF 2.5D does not solve a lid yet, so this model has nothing of its own to compare with; said |
| Via | A padstack between its two layers. A rectangle as it is; anything else a circle of its area at its centroid; said. The GDSII file keeps the shape |
| Touching shapes and vias | One net, as the solve has them |
| Edge port, calibrated or de-embedded | Wave port (`port_type` to change) |
| Other edge port | Gap port, its port solver off |
| Calibrate on a gap port | Gap port with its port solver on; said |
| Return `beside` | Reference edges on the line of its edge, by `besideEdges`, as the solve finds them |
| Named returns | Circuit port (`circuit:=` true), `AddCircuitRefPort` on each return's nearest edge |
| Internal port | `CreateCircuitPort` at its point, against the metal below it or the ground |
| Impedance | `Renormalize Impedence` |
| De-embed | `Deembed Distance` |
| Sweep, linear | `LINC from to points` |
| Sweep, log | `SINGLE` at each |
| Adaptive frequency | The top of the sweep (`adaptive_frequency`); Delta S 0.02, 20 passes |
| Margin | 5 heights of the stack (`margin`) |
| Air | A quarter wave at the adaptive frequency (`air`) |

### Where the two solvers do not have the same thing

Each is in the export's `notes` where it applies, in the script's head, and in the menu's box.

- **Lateral infinity.** RF 2.5D's stack has no end across the plane; the one in HFSS stops. Make `margin` and `air` larger if S moves.
- **Sheets against thickness.** A sheet has its thickness in its loss alone; HFSS has it in the field. `thin_metal` or the
  model's Solved as "faces" brings them together.
- **Terminals.** A probe's tips land on a pad's area here; a circuit port's terminal is an edge or a point there.
- **Internal ports.** A rectangle's cells here, its point there.
- **Reference planes.** RF 2.5D calibrates by strips of its feed; HFSS by its port solver.
- **Differential pairs** are not set up there: compare the ports' S, from which the modes follow.
- **Roughness.** Hammerstad's factor here, Groisse there, on the same rms height.
- **Vias.** Solid: a via's Filled fraction is not written. Round ones are circles of their area, where the mesh here may have had
  other sides.
- **The adaptive sweep.** A study that adds points writes the points it asks for.

### Import

| HFSS 3D Layout | RF 2.5D |
|---|---|
| Lowest signal layer, under other metals, one shape of it covering the design with nothing cut from it | The ground plane, without end; said. Perfect, or Metal where it has a finite conductivity. Anything else stays a metal: a coplanar line's signal and grounds cover nearly all of their layer |
| Other signal layer | A metal at its height |
| A signal layer in a run of them each on the last, in turn | A via between the two either side, its shapes the via's. A run takes turns from its bottom, and its top is always a metal. Sky130's `via2`, which has no height, comes in its place |
| Dielectric | A dielectric, cut by the metals in it (an Overlap stack's goes round them), the lowest a substrate with its conductivity |
| A via layer's fill | A dielectric of its own, unless a dielectric of the project is already there |
| What is left between layers | A dielectric of vacuum |
| Layers that overlap | Each here stands on the last, so the stack comes out taller; said |
| Rectangle, circle, path, polygon | A polygon, arcs as chords; said |
| Void | Cut from its owner with gdstk's union, as a keyhole |
| Rounded rectangle | Square corners; said once |
| Via through several metals | A via between each two neighbours (`V_a_b`); said |
| Padstack's pad on a layer the via passes | A polygon on that metal: circle (16 sides, its area), square or rectangle, turned with the via. Any other shape: left out, the barrel's width instead; said. A surface pad, with no hole, is its pad alone. Where it overlaps metal it merges |
| Padstack's anti-pad | Cut from each shape of another net than the via's that it reaches (a plane's outline is often whole, and this alone keeps a signal off it); said. A bottom layer so cut is no ground plane |
| A padstack's own layer names (`s1`, `s2`...) | In the stack's order over the via's span, upper first |
| Plated via | Solid, of the same conductivity; said |
| Edge terminal | An edge port on its layer at the edge's middle, facing out. `HFSS Type` Wave: calibrated (a gap port's port solver is not carried: set Calibrate if it wants that). Referenced on its own layer: it returns `beside`. On another metal: through a terminal on that metal under its point (`<port>_ref`), so not calibrated; said. On the plane, or nothing named: through the ground, said where there is none |
| Point terminal, via terminal | An internal port |
| `Deembed`, renormalization | De-embed, impedance |
| The cell | The one the `.aedt` names, else the one with the most shapes; a `_symbolFootprint_` only for want of another |
| Sweep | The study's from, to and points (`LINC`, `LIN`, `DEC`, `SINGLE`), from nought moved to the first step; said |

The stack may still want a look. A via layer with no height leaves its metals touching, which a stack here may not: the model
says so in `problems`, and it is the user's to give them room (sky130 does this). A design with no port opens without one, also
said.

## 5. What was checked, and what was not

### Not checked: nothing here has run in AEDT

**No Electronics Desktop has run any of this.** AEDT does not run on macOS and the user has none to hand for now. The script was
run against a Python stand-in that records every call and answers what the documentation says, which proves its logic and
its arguments' shape, not that AEDT accepts them. Nothing compares S with HFSS yet. That is the benchmark itself, still to come.

The `.aedt` reader's setup blocks are inferred too: the samples' setups in `.aedt` were empty (their setups in `edb.def` were not,
and those are checked).

**Look at these first, in this order, on the first run in AEDT.** The script's report names any call that failed, and goes on
past it:

1. `InsertDesign("HFSS 3D Layout Design", ...)`: the type's name, with `HFSS3D` as its fallback.
2. `AddStackupLayer`: the roughness keys (`RMdl`, `NR`, `HRatio`) and `ZeroMetalLayerThickness`.
3. `CreateEdgePort`: the edge numbers. The script reads each polygon back with `GetPolygon` and goes by that, and says when it
   differs, but whether `GetPoints` is in metres as PyAEDT has it is unchecked.
4. `ChangeProperty` on a port: `HFSS Type`, `PortSolver`, `Renormalize`, `Renormalize Impedence` (the documented spelling),
   `Deembed`, `Deembed Distance`.
5. `oPadstacks.Add`: the partial arguments for a padstack, and whether its pads must be larger than its hole.
6. `EditHfssExtents`: the arguments' shape, the extents' values, and a wave port's room inside them (`PEC Launch Width`).
7. `CreateCircuitPort`: its units.
8. `ExportNetworkData`: its arguments, in the `solve` part.
9. A gap port with reference edges: whether AEDT calls it Gap or wants something else for a coplanar port.

If the script stumbles, the `.gds` and `.xml` are the documented way in for the same stack and shapes, with circuit ports.

### Checked here

- **`test_rf25_hfss`** (no Ansys software and no file of theirs in the repository):
  - The script parsed by Python's own parser, and run against the stand-in, each command's arguments read back: stack, fills, planes, every kind of port, a keyhole as a void, vias and nets, edge numbers when a polygon reads back rotated, closed or moved.
  - The control file read as XML: its stack, ports and sweep as the model says.
  - Export then import: the same model, also with the layout gone (read from the `.gds` beside it, then from the script).
  - A project written by the test's own writer of the format, read as a model: the plane, the line, a void and an arc, a via with its pads and anti-pads, an edge and a point port and what each returns through, the sweep; a run of signal layers as metal and via in turn, in an Overlap stack; a footprint cell; records it does not know; an encrypted file; an `.aedt` beside it.
  - S beside a Touchstone file: the same, a port changed, the other reference, the order, between points.
- **What the small files could not find.** The test's writer only knows what its author thought the format was, so it passed
  with these wrong. Importing, solving and exporting that sample found them, and a test now holds each, on a small file or (the
  last two) a polygon by hand:
  1. An edge's shape id was read from the wrong field, which put a port on another layer.
  2. A via record's hole figure was taken as the barrel: 1 mm in a clearance of 770 µm, which shorted the planes.
  3. Anti-pads were not read, which shorted every plane a via passes.
  4. Holes chained by slits were left in the outline, all but the first of each chain.
  5. What stands in a void was given the net of the shape round it, on the way out.

  Driving the packaged app found a sixth, on a control file: a coplanar line's only metal, its signal and grounds covering 97 % of it, was taken for the ground plane, leaving nothing to solve. A plane is now one whole shape under other metals, on both routes.
- **A file damaged anywhere.** Every int32 of a small file as -1, the largest, a million and nought; every double of its records as NaN, infinity and 1e30, at every byte; the file cut at every length: 14,193 reads under ASan and UBSan with no fault, and a place that is no number stops it. Damaged projects also went on through to a model written to disk.
- **The public samples** read to their ends and made models, in Release and under ASan and UBSan (one sample's ports on their lines checked by name). Their pictures were looked at (`all.png`, not kept), and were right.
- **`test_qucs_control`:** the tools through MCP's own path (every argument, every refusal by name), the write guard on all three, and the menu driven through its file dialogs and boxes: suggested name, No and Yes on writing over, a refused file.
- **`test_rf25_doc`:** the three menu items, and when they are enabled. **`test_build_commit`:** the Trademarks tab.
- **The dry run** in section 2, on scratch copies; nothing under `~/QucsWorkspace` or the real cache changed (`find -newer`).
- **Vendor names.** HFSS™ joined the Trademarks in the README and the About tab, word for word; `check-third-party` now also refuses `.aedt`, `.aedtz` and `edb.def` files and an HFSS possessive.

- **145 breaks**, each undoing one piece and built and tested on its own, all caught (`breaks.py` and `results.txt` beside this file). The first run of 120 caught 92. Each of the other 28 either became a test or showed code that did nothing, which was taken out (a polygon's closing point, type checks the tool schema makes first). One, reading past the end of the file, only the sanitizers can see, so it runs under them.
- **The suites.** The full ctest in Release, 163 of 163. Run once more on the final code, at a load of about 20 from other sessions' work, two suites failed: `theCommitIsTheOneCheckedOut`, as another session's commit had moved HEAD after the build; and `aWaitingCallIsNotHeldBackByLaterOnes`, whose 500 ms margin had 282 ms that time, in code this does not touch. Both passed when rerun, the second five times more on its own. `test_rf25_hfss`, the new functions of `test_qucs_control`, and the changed ones of `test_rf25_doc` and `test_build_commit` under ASan and UBSan. The four lints green.
