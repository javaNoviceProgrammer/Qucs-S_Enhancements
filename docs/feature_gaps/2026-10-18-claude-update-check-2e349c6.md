# Update check of 2e349c6: the Lumerical round trip, surface recombination of T, the APD's tunnelling

The reviewer's check of `2e349c6` (`2026-10-14-claude-update-check-2e349c6.md`) found Lumerical's own projects importing and solving (its §1-§3). It left these open:
- its §4: a Qucs-S model sent to Lumerical CHARGE as a script and read back lost its graded dopings, its own models, its surface recombination and generation, and its mesh. The hand-built photodiode's dark current came back 366 times;
- surface recombination's temperature model was not imported;
- the APD's dark current with trap-assisted tunnelling (TAT) was 1.4 to 2 times Lumerical's.

The reviewer's files (`~/QucsWorkspace/lumerical_vpd`, `lumerical_apd`) were copied to scratch and run there through the build's tools.

## 1. The round trip (§4)

**Was.** `vpd.qfem`, exported to an .lsf and imported back, passed 61.4 µA at -1 V where the model passes 0.168 µA:
- **Its graded dopings dropped.** The export writes each as `addimportdope` with a dataset; the importer said "an imported doping profile (sampled) is not read".
- **Its own models** (Fossum's SRH, Masetti's mobility, its gaps and masses) came back as the library's: the script names Lumerical's database materials.
- **Its surface recombination and generation** were not exported.
- **Its mesh** came back as 84,000 triangles, the import's own rule, where the model's had 37,000.

**Now, in the script, as Lumerical's own objects:**
- **A doping by expression or implant**, as before: a dataset of N on a grid, finer where it changes.
- **A surface recombination** as `addsurfacerecombinationbc` between its two materials, with its velocities. Where they are S (T/300 K)^η in T, its temperature model is written too: `enable temperature model`, `electron eta`, `hole eta`. Lumerical's applies on every interface of those two materials, which is said when that is more than the model's own.
- **A generation** as `addimportgen` with its sampled dataset. Where G reads the model's parameters (a power swept), it is sampled at their values, and that is said: the reviewer's photodiode's `Pin` is 0 W there.
- **The solver's edges:** `min edge length` and `max edge length`, from the model's sizes.

**Now, for Qucs-S:** one line of JSON, `#qucs-s {...}`, a comment Lumerical passes over. It carries what the script cannot:
- the semiconductor's own features (its models, recombination, tunnelling, traps...);
- its settings (T, statistics...);
- the model's parameters, functions and mesh;
- the dopings, surface recombinations and generations again, exactly, each with the script object it stands for.

The import reads it back, and the exact feature takes the place of its sampled or widened object. Selections are written in the exported structures' names, as follows:
- **A domain rule** becomes its domains.
- **"between" and "only"** go by the domains each object owns, as the solver's selections do.
- **"objects" and "of"** go by the region each object covers.
- **A box** is kept in metres.

A rule by numbered boundaries or by a named boundary selection cannot be written so. Its feature is left off the line, and that is said.

**Without that line** (a script edited, or one written elsewhere), the import reads the script's own objects:
- **An `addimportdope` or `addimportgen` dataset** becomes a table of x and y (an interpolation function), applied within its data.
- **A surface recombination's temperature model** is read, the same as from a project (§2 below).

**Measured** through the tools on the reviewer's photodiode (scratch copies):

| | The model | Its round trip, before | Now |
|---|---|---|---|
| Dark current at -1 V | 167.1 nA | 61.4 µA | 167.2 nA |
| At +0.1 V | 957.7 nA | 75 µA | 957.8 nA |
| Every one of its 18 voltages | | | within 0.1 % |
| Triangles | 37,651 | 84,000 | 37,660 |

The MOS capacitor's round trip (`aChargeRoundTrip`) was 1.3 % apart at 2 V and twice the charge at -2 V, its mesh the import's own. It is now within 0.06 %.

## 2. Surface recombination of T (§1.2)

**Was.** A project's surface recombination with its temperature model on (the photodiode's Ge/SiO₂, η -3.8) came in as a constant 2250 m/s.

**Now.** Each carrier's S is S (T/300 K)^η (`enable temperature model`, `electron eta`, `hole eta`, read from a project or a script), and that is said.

## 3. The APD's dark current with tunnelling (§1.1)

**Not resolved.** What was found:

- **Our TAT is Hurkx's.** Γ = (ΔE/kT) ∫₀¹ e^(u ΔE/kT - K u^(3/2)) du is integrated as Hurkx defines it. `test_fem_semiconductor` already checks τ/(1 + Γ) in a uniform 2e7 V/m against that integral to 1e-3. This separates the two causes the reviewer suggested; Lumerical's own formula is not documented where it could be read.
- **On this build the gap is smaller.** The current, against Lumerical's, is 0.67 of it at -1 V and 1.39 at -4 V; the reviewer measured 0.71 and 2.04 at `2e349c6`. The nodes' boxes have changed since (Voronoi, recombination by domain).
- **Each mechanism turned off in turn**, the imported APD at -4 V (half domain; Lumerical's 5.6 nA):

  | On | Current |
  |---|---|
  | all | 7.80 nA |
  | no TAT | 4.02 |
  | no impact ionization | 6.03 |
  | no band-to-band tunnelling | 7.80 (none of it) |
  | none of the three | 3.10 |

- **The steepness is not TAT's.** From -1 to -4 V ours rises 76 times with everything on and 71 times with TAT, impact and BTBT all off; Lumerical's rises 37 times. TAT scales the current by about two at every bias. The steeper rise is in how the germanium depletes through the charge layer, and without Lumerical the conventions behind it (its doping's lateral roll-off, its mobility's driving field) cannot be pinned down.

## The review's notes

- **The majority-carrier switch** is not read, and that is said (§1.3).
- **The imported generation's selection includes silicon**, as Lumerical applies its data where it is; right, as the reviewer concluded (§1.4).
- **The APD's speed** needs no change (§1.5).

## Not done

- **The model's own semiconductor parameters in Lumerical's terms** (`setmaterial` of its mobility, SRH and band structure, the reviewer's §4.2 for Lumerical itself). Lumerical's script names for a CHARGE material's model parameters cannot be checked without Lumerical. A script that guessed them would build and quietly leave them out. The script keeps Lumerical's database materials and says to check them; the Qucs-S line carries the model's own back.
- **Mesh constraints in the script:** only the solver's two edges are written.
- **The APD's dark current**, as above.

## Tests

- **test_fem_lumerical `aGradedDiodeRoundTrip` (new).** A silicon diode with:
  - a Gaussian p layer by expression;
  - its own SRH (Fossum) and Auger;
  - surface recombination at its oxide, S (T/300 K)^-2;
  - a generation of a swept power;
  - its own mesh.

  The script's objects are checked: the dataset, the boundary condition with its temperature model, the imported generation, the solver's edges and the line. The model's features come back exactly, with:
  - "between" by owner: the oxide's two domains, not the metal its rectangle covers;
  - no library recombination beside its own.

  Its currents at -0.5, 0.5 and 0.6 V are within 1e-3 of the model's. At 0.3 V the light's current nearly cancels the diode's, so the two meshes (polygons back, 5,081 and 5,098 triangles) are 1.1 % apart there, and 0.1 % with elements half as large. Without the line: the doping and generation tables, and the surface recombination of T from the script's own objects.
- **test_fem_lumerical `aChargeProfiles`.** An implant back exactly with the line; without it, a table of its samples (it was left out, said).
- **test_fem_lumerical `aChargeRoundTrip`.** The gate charge within 0.2 % (it was 3 % of the largest).
- **test_fem_lumerical, the device project:** its Si/SiO₂ surface recombination with its temperature model. The fixture's generator writes it (`lumerical_projects.py`); the other fixtures are byte for byte as before.

## Breaks

Twelve changes broken on purpose, all caught (two after the test was made stricter):
- S(T) not imported;
- a sampled doping not read;
- no Qucs-S line written;
- the line not read;
- the replaced objects kept;
- no temperature model written;
- no generation written;
- the mesh not carried;
- the boundary condition's command not known;
- a generation's dataset not read;
- after the test checked the selection: "between" by coverage. It also put recombination on the anode's boundary, which changed no current, as an ohmic contact holds its carriers;
- after the test checked for it: the library's SRH beside the model's own. The model's own came later and won.

## Runs

- **Release:** the suite 129 of 129.
- **Under the sanitizers, with Eigen's LU (as on CI's Linux):** no report.
  - `aGradedDiodeRoundTrip` 72 s, `aChargeRoundTrip` 79 s, `aChargeProfiles` 8 s.
  - The two round trips first ran past QtTest's 300 s there, the round trip's mesh now the model's own. A sanitized build takes two voltages and elements twice as large, the model's and its round trip's alike, and allows 2 % between them: its coarse meshes, polygons back, are 1.3 % apart.
- **The lints pass.** The third-party check caught a possessive in a new note.
