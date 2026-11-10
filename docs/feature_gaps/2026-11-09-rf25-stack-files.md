# RF 2.5D's stack to and from HFSS™ 3D Layout's XML stackup and EMX's process file

The request: "Can you add import and export support for the 'Stack' from RF 2.5D tree to .xml file for hfss 3d layout and
emx .proc process file?"

Ansys® and HFSS™ are ANSYS, Inc.'s marks, EMX™ Cadence Design Systems, Inc.'s; the Trademarks section of the README says so.

## What was built

- **The Stack's menu** in the tree: **Import Stack…** and **Export Stack…**. The file's suffix says which format: `.xml` for
  HFSS 3D Layout, `.proc` for EMX. A name typed with no suffix takes the kind chosen in the dialog. A box then says what was
  written or read.
- **Two tools:** `rf25_stack_export {file}` and `rf25_stack_import {file}`. `file` is relative to the model.
- **The import is one Undo step.** It puts the file's stack in place of the Stack's children. It also updates the layer map
  for the layers the file draws by GDSII number: those rows replace the ones the map had for those layers or numbers. The rest
  of the model is left alone. Anything that now names a layer that has gone (a port, a row) is said.
- `rf25/rf25_stackfile.{h,cpp}` holds both writers and the EMX reader. The HFSS reader sits beside the control-file reader in
  `rf25_hfss_import.cpp`, which it shares, as does the routine that turns layers placed in z into a stack (`stackOfLayers`).

## The formats, and where they were read from

**HFSS 3D Layout.** The format's documentation. A stackup is a control file holding its `<Stackup>` alone: `<Materials>`, then
`<ELayers LengthUnit>` with `<Dielectrics>` (top first, each on the next), `<Layers>` (conductors at their `Elevation`),
and `<Vias>` (`StartLayer`, `StopLayer`). Layout > Layers > Stackup > Import XML reads one, and Export XML writes one.

**EMX.** The format's documentation. The layers come from the bottom up, each `layer`
being a height, a permittivity, then optionally a permeability and a conductivity, then `name`, `conductivity` and
`tan delta`. There is always a ground plane under the first layer, and the last layer is without end (`infinity`).
`conductor` gives a thickness, a conductivity and a region expression, and is placed by the `offset`s and `position`s
since the last `layer`; it may reach through several layers. `via` joins two conductors, or a conductor and `backside`
(the plane). `define` names GDSII layers (`L15T0`), region expressions, and numbers. `assume` sets units, for every
context or for one (`layer`, `conductor`, `via`, `region`). `begin flip` / `end flip` turns a part of the stack upside down.
`<<<` … `>>>` is an encrypted part. EMX reads names without regard to case.

## The mapping

### Out

| RF 2.5D | HFSS XML stackup | EMX process file |
|---|---|---|
| Lengths | µm (`LengthUnit="um"`) | `assume microns` |
| Substrate | Dielectric of its own material, conductivity kept | `layer t εr 1 σ name … tan delta …` |
| Dielectric | Dielectric layer | `layer t εr name … tan delta …` |
| Metal | Conductor at its elevation, `FillMaterial` the dielectric above it; its room a dielectric `<name>_fill` of that, so the dielectrics run unbroken (as rf25_hfss_export has it) | A `layer` of its own room, of the dielectric above it (else air), and the `conductor` in it |
| A metal's or via's GDSII layers | Not in a stackup: its layers go by name (said). rf25_hfss_export's control file carries the numbers | `define M1 = L1T0+L1T20`, from the layer map. No row: `# define M1 = L?T?`, said, and EMX stops there until it is written |
| Via | `<Vias>` layer, `StartLayer` the upper as Ansys writes it; filled fraction folded into the conductivity (said) | `via lower upper σ region`; to the ground `backside` |
| Ground: perfect | Conductor of conductivity 1e30, no thickness, under everything | Nothing: EMX's plane |
| Ground: metal | Five skin depths thick at the bottom of the study's sweep (1 GHz without one) | EMX's plane, perfect: its loss is said to be left out |
| Ground: impedance | A 1 µm sheet of the conductivity that gives it (reads back as a metal plane) | As metal |
| Ground: open | No plane | `layer H 1 name rf25_open_ground`, H = ten heights of the stack (at least 1 mm), over EMX's plane; said. It reads back as an open ground |
| Cover: open | Nothing | `layer infinity 1 name air` |
| Cover: perfect | A dielectric of air `<cover>_gap` and a conductor of 1e30 over it | `layer h 1 name <cover>_gap` and `layer infinity 1 conductivity 5.8e7`: EMX has no perfect lid, so copper |
| Roughness | Not in a stackup: said | Not in a process file: said |
| Names | As they are | Letters, digits and `_`, not EMX's words or `LnTm`, unique without case; a changed one said |

### In

Both readers turn the file into layers placed in z: dielectrics, and signal layers or conductors. The same routine as the
HFSS project import then cuts each dielectric where a metal takes room. Metals that overlap in z each stand on the last,
and this is said.

| From | RF 2.5D |
|---|---|
| HFSS: the lowest conductor, its bottom at the bottom of every dielectric | The ground plane: perfect where its conductivity is 1e20 or more or none, else metal. Here every metal stands on the substrate, so a metal at the very bottom can only be the ground. Said |
| HFSS: the highest conductor, perfect, over a dielectric of vacuum | A metal lid at that dielectric's thickness. A perfect metal on the top dielectric alone stays a metal: it is as likely a line of pec |
| HFSS: several streams to one `TargetLayer` | One metal, a row of the layer map for each stream |
| HFSS: a conductor named by a number (a GDS control file) | A row of the layer map: number, `GDSDataType`, its target |
| HFSS: a whole control file | Its stack alone; the rest is left, and said |
| EMX: the plane | The ground, perfect, named GND (EMX's plane has no name) |
| EMX: first layer `rf25_open_ground` of εr 1 | An open ground, the layer left out |
| EMX: first layer | The substrate, with its conductivity |
| EMX: another layer's conductivity | Said: only a substrate's is solved |
| EMX: permeability ≠ 1, magnetic loss tangent | Left out, said |
| EMX: the last layer, εr 1, no conductivity | The air above: no cover |
| EMX: the last layer, conducting | A perfect metal lid over the top (its conductivity said) |
| EMX: the last layer, another dielectric | A dielectric as thick as the stack below it, the air above; said |
| EMX: a sheet resistance (ohm/sq) | The conductivity that gives it at the conductor's thickness |
| EMX: resistivity (ohm-cm) | 100/ρ S/m (ρ of nought: no conductivity) |
| EMX: a via in ohm/via, or by size alone | The via's own default conductivity, said: a resistance a via is not a conductivity |
| EMX: a via | Between its conductors, from the lower (the ground where it is one), placed just under the upper |
| EMX: a region of `+` and `count`/`nolabels`/`hide` | Its GDSII layers as rows of the layer map |
| EMX: a region that grows, merges, cuts or intersects | Its first layers as rows, the rest said. A layer drawn for two (`L16T0` for three vias) goes to the first, said |
| EMX: `random` | Nought, as EMX takes it outside a Monte Carlo run |
| EMX: `+/-`, `-/+` | Left out: the nominal value |
| EMX: `table` | Its first entry, said |
| EMX: `deltat`, `dtemp` | Nought (the nominal temperature) |
| EMX: a name defined nowhere | Refused, naming it. Taken as nought only where part of the file is encrypted, and said |
| EMX: `begin flip` … `end flip` | Turned upside down as EMX turns it, nested as it nests |
| EMX: `capacitor`, `resistor`, `temperature`, `geometry scaling`, `emxmask`, a conductor's `bias` | Left out, said once each |
| EMX: an encrypted part | Not read, said |

Reading is strict, as the project import is. A statement it cannot read stops it, with the line and why: no number where one
is wanted, a word no statement has, a layer above the endless one, a flip not closed, a unit EMX does not have, a sheet
resistance on a conductor of no thickness, a name defined by itself.

## What was checked, and what was not

**Not checked: neither Electronics Desktop nor EMX has read a file written here.** Neither runs on macOS, and neither is
here. The stackups follow documented examples, attribute for attribute. The process files follow the format's grammar.

- **`test_rf25_stack`** (15 functions):
  - **Out:** the exact lines and layers of each file written, for a two-metal process with a via, a lossy substrate and a
    layer map.
  - **Round trips:** each format reads back as the same stack. Grounds (perfect, metal, impedance, open) and a lid go there
    and back in both formats.
  - **EMX's grammar** on a process written for the test: units by statement and by context, ohm/sq and ohm-cm, offsets and
    a position, a conductor through two layers, regions by difference and intersection, a via to the backside, attributes.
  - **The rest:** variations, `random`, `kA` units, a flip, an encrypted part, a via of sizes. A stackup as documented
    examples have it. Refusals by name. Every cut of a file read without fault. The model's other parts left as they were.
  - **The control-file import keeps each permittivity**, which it did not (below).
  - With `QUCS_EMX_SAMPLES` set, every `.proc` under that folder is read, and its stack is written as EMX and read back the
    same.
- **`test_qucs_control`:** `anRf25StackGoesOutAndBackThroughItsToolsAndItsMenu`. Both tools both ways, refusals,
  one step of Undo, and the files read back as the stack. It also drives the Stack's menu through its dialogs and boxes:
  the suggested name, a name with no suffix, a file that cannot be read.
- **38 breaks**, each undoing one piece, built, and its tests run; all are caught (`breaks.py` and `results.txt` beside this
  file). The first run caught 34. Each of the other four became a test, or showed code that did nothing, which was taken out:
  the EMX reader's own layer for the plane, which the shared routine makes from the plane's name alone.
- **Lints:** check-tool-arguments, check-third-party and check-json-nesting are green.

### Two bugs of the existing HFSS import, found on the way

1. **Permittivity.** The control-file reader took a `<Permeability>`'s value as the permittivity, because it comes after
   `<Permittivity>`. rf25_hfss_export writes both, so every dielectric of a control file imported through
   rf25_hfss_import came in at εr 1.
2. **Duplicate metals.** Several streams to one `TargetLayer` (a documented example) made one metal of each.

## The check of 9 November

`qucs-reviews/RF2p5D/2026-11-09-claude-rf25-stack-roundtrip.md` took a 15-node stack out and back in both formats, through
the built app. Everything either file can carry held, and a second pass was stable. It found three things.

- **S1: a metal's "Solved as faces" came back a sheet, and nothing said so.** That changes answers: the wall charge, and
  the effective permittivity of thick lines. Neither file has a place for it, since each solves a metal at its thickness.
  - **On export:** a note now names the metals solved as their faces, as it already did for roughness.
  - **On import:** a metal keeps its `model` and `roughness` from the metal of the same name in the stack being replaced,
    and the box says which. EMX's spelling of a name also counts (`M3_thick` for "M3 thick"). A metal with no namesake
    comes in as a sheet, as before.
- **S2: some of EMX's renames went unsaid.** Every rename is now said: dielectrics and the substrate as well as metals and
  vias, and a ground that comes back as GND because EMX's plane has no name.
- **S3: a metal lid is offered but not solved** (`rf25_green.cpp`). This is a gap in the solver, not in the round trip,
  and it is left as it is.

`whatNeitherFileSaysOfAMetalIsKeptByName` holds S1 and S2, and six more breaks (44 in all) are each caught.

**The recheck after `507865e7`** found S1 and S2 fixed, and one small thing left. A file imported into a model with no metal
of the same names brought its metals in as smooth sheets, as designed, but said nothing of it, so a file written elsewhere
gave no hint. The import now says it: "M2, M3 thick came in as sheets with no roughness: the file says neither, and no
metal of those names was here to keep them from." A metal whose namesake was here is not listed, even where that namesake
was a sheet too. The same test holds this, into a blank model (whose own M1 is a namesake) and into a stack with no metal at
all; two more breaks (46 in all) are each caught.

