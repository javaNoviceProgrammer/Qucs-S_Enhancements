# Feature gaps: Lumerical's projects in and out, the update check

*14 October 2026 — on `d959d51`. The review: `2026-10-13-claude-update-check-lumerical-io.md`,
13 October, on `d959d51`.*

The review imported Lumerical's own example projects from the day's benchmarks and
compared each with its hand-built model and with Lumerical's results. Of seven files, two
worked: the plain MODE waveguide and, with hand fixes, the MOSFET. The others failed:

- the LN ridge (a structure group's script) did not read;
- the thermal tuner's core became n = 1;
- the photodetectors lost their pyramids, materials and a doping;
- the HEAT project was refused with the MQW message.

The CHARGE export also dropped a thin doping peak.

All seven now import and solve. Most of the numbers below were measured on the real
files, and the tests check the same things on files made to Lumerical's layouts.

| File | Review | Now |
|---|---|---|
| `Waveguides_FDE.lms` (MODE) | Works: TE0 2.502, TM0 1.839 | Unchanged. Its setup script is said not to have been run: the structures are as it left them when the file was saved. |
| `waveguide_fde.lms` (thermal tuner) | The core (`silicon_thermal`, kind 13) n = 1 | **Read.** The core is Palik's silicon at 1550 nm (n 3.4764), with dn/dT 1.8e-4 /K from 300 K at the FDE's 300 K, as a wave feature's thermo-optic term. TE0 is 2.35364, against the hand model's 2.35320 at 0 mW. |
| `LNO_waveguide.lms` (LN SHG) | "its objects do not read (at byte 340277)" | **Read.** The group LNOI's ridge (a polygon of 8 vertices) and BOX are read with the diagonal index 2.25876; 2.1795; 2.25876, which its setup script set for 775 nm and 323 K. The FDE runs at 775 nm with its sweep to 1500 nm. Mode 7, Lumerical's TM2, has neff 1.93861, against the hand model's 1.9386 at 323 K. The ten frequencies take 8 s. |
| `nmos.ldev` | I_d 2 to 2.5 times Lumerical's | **Imported as it is:** 0.311, 2.77, 4.08 and 9.13 µA at V_g 0.4, 0.8, 1.0 and 1.8 V, against Lumerical's 0.368, 2.82, 4.09 and 8.83 (before: 3.71, 7.22, 8.96 and 15.8). |
| `vpd_CHARGE.ldev` | Unusable: the pyramids lost | **Imported as it is:** dark current 0.335 µA (Lumerical's 0.344), responsivity 0.935 A/W (Lumerical's 0.945). |
| `avalanche_photodetector_electrical.ldev` | Unusable: the pyramid, three materials and a doping lost | **Imports.** Without trap-assisted tunnelling its dark current is 0.93 and 1.05 of Lumerical's at -2 and -4 V. With it, as the file has it, it is 1.43 to 2.04 times Lumerical's at -2 to -4 V (open: below). |
| `waveguide_heat.ldev` (HEAT) | Refused as "not a Lumerical MQW project" | **Imported:** 0.7224 K/mW, against Lumerical's 0.7151 and the hand model's 0.7228. |

One more `.lms` from the benchmarks, `vpd_absorption_length_optimization.lms`, was not in
the review. It now reads its pyramids and stops at its solver with a plain message: its
FDE is not 2D Z normal (MODEType 0), and only that one is read.

## The review's items

| Review | Now |
|---|---|
| 1. The gate's work function convention (a bug of the importer): a semiconductor's work function in the Lumerical software is to the intrinsic level, which the importer did not allow for, so 3.64 eV copied into `phim` shifted the threshold by -0.4 V. | **Fixed by reading χ in Lumerical's convention.** A semiconductor's χ is taken from its work function: χ = W - Eg/2 - kT/2 ln(Nc/Nv), with Eg (Varshni's) at the solver's temperature and Nc, Nv from its masses. A metal's work function, the gate's, is kept as it is. The NMOS's silicon (W 4.2 eV) has χ 3.639 eV, so its n⁺ gate's 3.64 eV puts its Fermi level at Ec, as in Lumerical. **Test:** an imported MOS stack is flat at Lumerical's flat band voltage, W_gate - (W_si + kT ln(NA/ni)). |
| 2. The material models are the library's: import them as `model`, `srh`, `auger`, `radiative`, `impact` and `btbt` nodes. | **Done.** Each semiconductor's models are read from its record's groups (the active model of each, on only if it and the groups it is in are on). They give the material's Eg, χ, Nc, Nv, εr, mobilities and lifetimes, and its features: Masetti's or Caughey and Thomas's mobility by doping; saturation with each carrier's β; Slotboom's band gap narrowing; Fermi-Dirac statistics; SRH with Fossum's doping dependence, Hurkx's tunnelling and its trap level; Auger; radiative; Selberherr's impact ionization; Hurkx's band-to-band tunnelling. Four of these were not in Qucs-S and are new (below). |
| 3. Pyramids are not read: cut by the plane, a pyramid is a trapezoid. | **Done,** in `.ldev` and `.lms` (shape 9 there: its x, y, its spans at its foot and top, z min and max). Cut across its axis it is a rectangle at that height's spans. |
| 4. The APD file's material ids unresolved (DEVICE 7.17, format 6.5). | **Done.** The project's own material objects are read. Each has its electrical and thermal records under its children's `Custom/1/<id>`, its geometry refers to it by uuid, and its name is the record's (its object's name an alias, which surface recombination uses). |
| 5. p⁺⁺Ge doping dropped: a box equal to its junction width refused. | **Kept.** Its plateau is a box of no depth at its source face, with Gaussian tails of the junction width. |
| 6. Not read: surface recombination objects, the imported generation, monitors. | **Read.** Surface recombination between two materials is the new Surface Recombination feature on the boundaries between them (each carrier's velocity, the trap's offset). At a metal contact it is left out, and said: the carriers are at equilibrium there already. The imported generation is a function of x and y, the slice of its data at the plane, times its scale factor. Saved off (dark), it is off. Monitors are counted and left out. |
| 7. Temperature-dependent optical materials (kind 13): their index at the wavelength and T0. | **Done:** the base material's index at the wavelength, with dn/dT (T - Tref) as a thermo-optic term at the FDE's temperature. A heat physics' ht.T in its T tunes it. Its free carriers' (Soref's) terms are said and not taken; there are no carriers in a MODE model. **Sellmeier materials (kind 6)** are read too: their index along x, y and z at the wavelength, diagonal where they differ. |
| 8. The MOSFET plots `semi.I_gate` (always 0). | **Fixed.** A gate sweep plots the other contacts' currents: `semi.I_drain` (those at a voltage, else all but the gates). |
| 9. Import to a folder that does not exist fails. | **Fixed:** the folder is made. |
| 9. The first imported model reported unsaved right after the import. | **Fixed.** A model whose write fails is closed, not left open, unsaved, under the file's name, where a later import or write of that file met it. |
| (§3) The HEAT project's message. | **HEAT projects import** (a ThermalSimObject): its materials' thermal records (conductivity, constant or of T, density, heat capacity; its fluids not solved), temperatures and convection on the region's faces or between two materials, heat sources over the norm length. |
| (§3) `LNO_waveguide.lms`: the ridge built by a structure group's script. | **Read as saved.** A structure group's values are all tagged, then its script and its own properties. Its children are the structures its script made, saved with it: without their kind and id, and the polygon without a count of children. They are placed from the group's place. The scripts (the group's, the model's setup) are not run, which is said: the saved state is theirs when it was saved, the index at 775 nm with the FDE at 775 nm. |
| (§4) The CHARGE export samples an expression's doping on a fixed grid (26 nm apart): the p⁻ layer's peak 8e12 for 4e15 cm⁻³; its interface sample written as 0. | **Fixed.** The grid starts evenly (20 to 200 lines an axis, as before) with lines at the doped domains' corners. Each interval is halved while the profile midway is more than 1 % off the straight line between its ends (Lumerical's interpolation), along lines across it. A step at the selection's edge is not halved for: the edges have their own lines. An edge's sample is taken from a hair inside the selection. The hand VPD's p⁻ layer is now 214 x 61 points: its peak 4e15 cm⁻³ is on the interface, with 32 lines within 3σ of it. The lines are written to the last digit. |
| (§4) All domains share mesh order 3. | **Fixed:** each structure has its own order, the later drawn the lower, and so do the contacts' metal strips. The MODE export's too. |

## New physics

These came in with the importer, which needs them, and are features of their own.

- **Masetti's mobility by doping** (Model's Mobility "By doping (Masetti)"): µmin1 e^(-Pc/N) +
  (µ - µmin2)/(1 + (N/Cr)^α) - µ1/(1 + (Cs/N)^β), with silicon's arsenic and boron figures
  by default. Lumerical's is this with µmin1 = µmin2 and Pc 0.
- **Saturation exponents** βn, βp: µ0/(1 + (µ0 F/vsat)^β)^(1/β). Before, it was 2 for both.
- **SRH lifetimes** of x and y (a lifetime map), and **by doping** (Fossum's τ0/(A + B N/N0 + C
  (N/N0)^E), Lumerical's α, β, γ, σ).
- **Trap-assisted tunnelling** (Hurkx's): each lifetime τ/(1 + Γ), with Γ his integral ∫ e^(u -
  K u^(3/2)) du up to the trap's depth over kT, at the field at the node. The integral is
  taken by Simpson's rule, with its derivative for Newton's method.
- **`semi.taun`, `semi.taup`:** the lifetimes as solved.
- **Surface Recombination**, a boundary feature: (n p - ni²)/((n + n1)/Sp + (p + p1)/Sn) per
  area, a level of no charge. Interface Traps is the charged kind.
- **Interpolation of two arguments:** a table of every x with every y, bilinear between them
  and constant beyond. An imported generation map is one.

## Found on the way

- **Lengths in an imported box's expressions** (from `3701196`): a box doping's and a
  generation's bounds were bare numbers. Bare numbers are SI, so `y >= 0.2` was metres and
  the doping was nought. They now carry their unit.
- **Properties' named matrices:** a matrix entry of type 3 or 5 begins with a name. Read
  without it, a collector contact and an APD's generation were lost.
- **An older generation blob**, a Properties matrix's layout after its name, as the VPD's
  is. Both layouts are read.
- **A surface recombination object with no materials** matched every material without an
  alias, and was taken as being at a metal. It is now said to have no materials.
- **Notes:** a constant band gap was called "(Varshni's)", and a solver without mesh edges
  said "Lumerical's edges 0 to 0 µm".

## Open

- **The APD's tunnelling.** With Hurkx's integral, the node's field from its own domain's
  triangles and the file's own trap masses, the APD's dark current is 1.43, 1.67 and 2.04
  times Lumerical's at -2, -3 and -4 V. Without the tunnelling it is 0.93 and 1.05. Either
  Lumerical's field for it is not the node's, or its Γ is not this one. Neither can be
  checked here without Lumerical. The model and its numbers are as Hurkx's paper has them,
  checked on their own (below).
- **Not done:**
  - impact ionization driven along the current (its driving field is read, and |E| taken);
  - Lumerical's majority-carrier switch for surface recombination;
  - the scripts of structure groups and of the model (the saved state is taken);
  - a temperature import's data (a script's, not saved with the project);
  - X- and Y-normal and varFDTD MODE solvers;
  - dκ/dT, said when it is not 0.

## Tests

- **`test_fem_semiconductor`:**
  - **`masettiMobility`:** electrons at 1e17 and 1e20 cm⁻³ (where µ1 counts), holes at 1e17
    (where Pc counts): semi.mun and semi.mup to 1e-6 of Masetti's, and the bars' currents to
    3e-3.
  - **`saturationExponents`:** at 1e7 V/m, the electrons' β 1 and the holes' 1 and 2: each
    current q N v to 3 %.
  - **`srhLifetimes`:** τ0 of x by doping, its own N0s and A, B, C, E: semi.taun and semi.taup
    to 5e-3. Tunnelling at 2e7 V/m: τ(1 + Γ) to 1e-3 of τ, with Γ integrated here on its
    own (200 001 points); without it, τ.
  - **`surfaceRecombination`:** n-type with a uniform generation, its end a surface of S, a
    contact 60 µm away: Δp at 0, 5 and 30 µm, the analytic profile's to 5e-3.
- **`test_fem_engine` `expressions`:** a table of two arguments, bilinear between its points
  and constant beyond. A model's grid in any order and its units. A table that is not a grid
  refused.
- **`test_fem_lumerical`:**
  - **`aModeProject`** (its fixture has new records):
    - a Sellmeier material's index along each axis (Zelmon's MgO:LN at 1550 nm);
    - a temperature dependent material's (its base's, dn/dT, at the FDE's 310 K);
    - an index along x, y, z;
    - a pyramid's cut, and a structure of a kind not read said;
    - a structure group's polygon and rectangle from its place;
    - mesh orders, the structure's own and its material's;
    - the notes on the scripts and the temperature import.
  - **`aDeviceProject`** (a DEVICE 7.17 file): each model above, read from the material objects:
    - χ from the work function, a model off where its group is;
    - the pyramid's trapezoid;
    - the diffusion box as wide as its junction;
    - surface recombination by the objects' names, and at the metal left out;
    - the generation table, scaled and off;
    - the gate's work function, the substrate's current plotted.
  - **`aMosFlatBand`:** the imported MOS stack at Lumerical's flat band voltage, -0.9745 V. The
    gate's charge there is -6.3e-18 C, against -2.2e-15 and 1.3e-15 C half a volt either side.
  - **`aHeatProject`:** the materials' records and the boundaries; solved, the heater at
    301.10531 K, against 301.10521 K from q (t_ox/k_ox + t_si/k_si).
  - **`anInterfaceDopingSampled`:** the review's p⁻ layer on a germanium layer:
    - its peak sampled on the interface;
    - 20 lines within 3σ of it;
    - the straight lines within 1.5 % of the peak everywhere;
    - each structure its own mesh order.
  - **`aModeScript`:** the MODE export's orders distinct.
  - **`aChargeProject`:** the empty surface recombination said.
  - **`importsRefused`:** a file cut short in its materials and in its objects.
- **`test_multiphysics_doc` `claudeImportsLumerical`:** the DEVICE 7.17 and HEAT files
  imported. An import to a folder not there makes it. A write that fails leaves no tab.

The fixtures (`tests/data/lumerical_projects.py`) are written to the layouts read from
Lumerical's files: the `.lms`'s tagged records, a group's children, a structure's tail and
its mesh order flag, the DEVICE 7.17 material objects and model groups, both blob layouts.

## Breaks

Each change was broken in turn and its tests run: 43 breaks.

| Of | Breaks |
|---|---|
| the semiconductor | Masetti without its µ1 term, without e^(-Pc/N); the electrons' β always 2, the holes' always 1; Fossum without B, the holes' N0 the electrons'; a lifetime of x not evaluated; Hurkx's K a third larger, the holes' tunnelling mass the electrons'; the surface's velocities swapped |
| the table | its axes swapped; a grid's y not in its unit |
| MODE's project | a group's place not added; a polygon's vertices read in halves; a pyramid's spans in another order; the mesh order flag the other way; Sellmeier without its poles; a thermal material at its reference temperature, its base not found; an index along x alone |
| DEVICE's projects | χ the library's (two tests), χ to midgap; Masetti's µmin2 nought; the material objects not read; pyramids not read; a box as wide as its junction refused; the surface velocities swapped; the generation not scaled; a group that is off read as on; an older blob's numbers 4 bytes off; the swept gate's current plotted; a surface recombination of no materials taken at a metal; HEAT's power doubled, its conductivity's exponent negated |
| the export | an edge's sample its own point's; no interval halved; no even lines; the lines rounded; the CHARGE domains one order, the MODE ones one order |
| the app | an import's folder not made; a failed import's tab left open |

Three first passed:

- **Masetti without its µ1 term.** At 1e17 cm⁻³ the term is µ1/(1 + (Cs/N)^β) = 43.4/(1 +
  3430²), nothing. Electrons at 1e20 cm⁻³ are now checked too, where it is 5 % (60.72
  against 64.12 cm²/Vs), and the break is caught.
- **A pyramid's spans in another order.** The fixture's pyramid had the same y span at its
  foot as x span at its top. Its four spans now differ (the same cut), and the break is caught.
- **The mesh order flag the other way.** The expected drawing order was the objects' own,
  so taking the material's order for every structure changed nothing. The core now has its
  own order 1 and must be drawn after the material's 2s, and the break is caught.

The other 40 were caught, and with these all 43 are.

## Builds

The release of v27.0.0 (on `d959d51`) did not build on Linux. GCC 12 (the release's) and
13.3 (CI's) stop compiling `fem_lumerical_import.cpp` from `3701196` with an internal
compiler error in `gimplify_var_or_parm_decl`, "in lambda function". The failing lambda is
buildCharge's `assemble`, at its first naming of a variable it captures implicitly, deep
in a loop. The fix is a commit of its own before this one, `d7ae967`:

- the Lumerical code's lambdas of 15 lines or more name their captures (by reference, as
  before), so GCC makes each capture's proxy at the lambda's start;
- arrays of QString from constant brace lists (GCC's PR c++/117985) and structured
  bindings that a lambda uses are taken out.

CI's Linux job compiles it. This round's code is written the same way: its 17 long
lambdas name their captures.

The full ctest (119 tests) passes. Under the sanitizers:

| Test | Time |
|---|---|
| `test_fem_lumerical` | 185 s |
| `test_fem_engine` | 5 s |
| `test_multiphysics_doc` | 442 s |
| `test_fem_semiconductor` | 906 s (918 s before this round) |

Two of the new semiconductor functions took 227 and 223 s there, too near QtTest's 300 s
on CI's slower runners. On a sanitized build they now mesh their uniform bars coarser and
leave out a redundant case: 33 and 3.5 s.
