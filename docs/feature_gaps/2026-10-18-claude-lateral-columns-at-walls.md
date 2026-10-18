# A Layered Region's columns at a material's side wall

The user (2026-10-18): *"fix the lateral mesh dependence too"*. This is the dependence found while answering the update check of a690d27 (`2026-10-17-claude-update-check-a690d27.md`, "found on the way").

## Was

The reviewer's toy laser (a GaAs disk 300 nm wide in Al0.6Ga0.4As, axisymmetric, 128 rows across each layer) passed a current set by the Layered Region's lateral element size, not by the device:

| Columns | Without its laser, 1.4 V | As a laser, 1.7 V | Triangles |
|---|---|---|---|
| 200 nm | 325.9 nA | 57.76 µA | 5,376 |
| 100 nm | 258.9 nA | 41.75 µA | 9,984 |
| 50 nm | 229.9 nA | 35.60 µA | 18,432 |
| 25 nm | 208.8 nA | 31.82 µA | 36,864 |
| 12.5 nm | | 30.21 µA | 73,728 |

The error falls with the column width, toward about 190 nA and 29.5 µA. A user who meshes such a laser at the usual 100 nm gets 40 % more current than the device passes, and nothing says so.

## The cause

The solver is a finite-volume scheme: each node owns a box (its dual cell), and the recombination and stored charge in that box are counted at the node's own carrier densities.

- **A node on the disk's side wall** takes the narrower gap's medium (GaAs), so its carriers are the active layer's.
- **Its box reaches half a column into the cladding beside the wall.** With 100 nm columns, a 50 nm strip of Al0.6Ga0.4As beside each wall recombines as if it held the disk's electrons and holes.
- **So the disk is effectively a column wider,** and the current goes as 1 + h/W, with h the column width and W the disk's width.

The first write-up put this down to the current funnelling into the disk through the claddings. It is the wall's nodes: grading the columns at the walls alone, the rest as wide as before, removes it (below).

The VCSEL example shows a second case of the same kind: its current crowds at the oxide aperture's edge (AlAs against AlOx), and even columns resolve that edge only as finely as they are wide.

## Now

A Layered Region's columns are graded toward a material's side walls (`fem_mesh.cpp`, the layered grid's columns):

- **A wall** is a boundary across the layers between two of a semiconductor's domains of different materials: a heterojunction's side wall, or an insulator's edge inside the semiconductor (an oxide aperture's). Two material entries of the same kind and properties count as one material. Without a semiconductor, or between domains of one material, the columns stay even, as before.
- **The columns there** start at the wall's size, and each is twice the one before, up to the lateral size. Each column leaves at least as much room again after it, so the even rest never has a column narrower than the last graded one (a 100 nm gap between two walls gets 3.1, 6.3, 12.5, 56.3, 12.5, 6.3, 3.1 nm, with no sliver in the middle).
- **The wall's size** is a new Layered Region property, "Lateral element size at its side walls" (`hwall`). Empty, it is a thirty-second of the lateral size. Set it to the lateral size to get even columns, as before.

The toy, graded:

| Columns | Without its laser, 1.4 V | As a laser, 1.7 V | Triangles |
|---|---|---|---|
| 200 nm | 194.7 nA | 29.76 µA | 16,896 |
| 100 nm | 192.4 nA | 29.57 µA | 23,040 |
| 50 nm | 191.5 nA | 29.52 µA | 33,024 |
| 25 nm | 191.2 nA | | 51,456 |

The current at 200 nm columns (16,896 triangles) is now closer to the limit than at 12.5 nm even columns (73,728).

**The VCSEL example** (`vcsel_laser.qfem`): its one wall is the aperture's edge at 2 µm. At 1.9 V:

| Columns | Current | Light | Threshold | Slope |
|---|---|---|---|---|
| 100 nm, even (as before) | 1.418 mA | 0.821 mW | 45.42 µA | 0.5976 W/A |
| 50 nm, even | 1.398 mA | 0.810 mW | 45.28 µA | 0.5982 W/A |
| 25 nm, even | 1.388 mA | 0.804 mW | 45.20 µA | 0.5984 W/A |
| 100 nm, graded (now) | 1.382 mA | 0.801 mW | 45.01 µA | 0.5986 W/A |
| 50 nm, graded | 1.383 mA | 0.802 mW | 45.11 µA | 0.5986 W/A |

The resonator shares the mesh: its mode is now 980.273 nm with Q 15,874, where it was 980.290 nm and 15,827. Γ and the threshold gain move under 0.3 %.

## The heterojunction warning

The coarse-mesh warning of the a690d27 check is unchanged. One of its cases now reads differently. The GaAs stripe in Al0.6Ga0.4As, swept to 1.6 V on 4 rows, named the p cladding's 120 nm rows at its last point and now names the n cladding's 75 nm rows. With its walls resolved, the holes enter the stripe through its sides, and its top interface carries 7 % of the current crossing, under the warning's tenth.

## Tests

- **test_fem_semiconductor `itsColumnsGradedAtSideWalls` (new).** A 2D GaAs stripe 100 nm wide in Al0.6Ga0.4As, 32 rows a layer:
  - its columns' lines exactly: graded from both walls; with `hwall` 10 nm, 10 and 20 nm from each wall; even with `hwall` the lateral size, with a stripe of the cladding's material (a second entry of it), and without a semiconductor; graded at an insulator's edge;
  - its current at 1.4 V: graded, 12.61, 12.68 and 12.70 mA (a metre of it) at 100, 50 and 25 nm, within 1 % of each other and 2 % of the limit, about 12.65 (even columns 1.6 nm wide give 12.57, graded to 3.1 nm 12.64). Even at 100 nm it is 19.34. A sanitized build leaves out the 25 nm solve.
- **test_fem_semiconductor `aCoarseMeshAtAHeterojunctionIsSaid`.** The stripe on 128 rows is not said with even columns or graded ones, and the sweep's last point names the n cladding's 75 nm rows.
- **test_fem_laser `theVcselLaserExample`.** Its mode, current, light, threshold and slope as above. Its lasing points' mean gain is checked against its own threshold gain, as G τph exactly and to 0.2 %. It was checked against a fixed 4.0905e4 per metre, which the mesh moved by 0.3 %.
- **test_fem_laser `aToyLaserAsItsRateEquations`.** The same identity: just above threshold, at 153 photons, the clamped gain is 1.9e-4 below the threshold's (β Rsp / S). The test asked for 1e-4 before.

## Breaks

Seven changes broken on purpose, all caught by `itsColumnsGradedAtSideWalls`:
- the walls not graded;
- the first column a sixteenth of the lateral size;
- any two domains a wall, whatever their materials;
- two entries of one material taken as two materials;
- the sliver back (the graded columns not leaving as much again);
- `hwall` not read;
- walls outside the semiconductor too.

An eighth, walls at the stack's outer edge counted too, changed nothing: a boundary between two of a stack's own domains cannot lie on its outer edge. That check was taken out of the code.

## Runs

- **Release:** the suite 129 of 129.
- **Under the sanitizers, with Eigen's LU (as on CI's Linux):** no report.
  - `itsColumnsGradedAtSideWalls`: 0.6 s. It meshes alone there; its graded solves took 70 to 100 s.
  - `aCoarseMeshAtAHeterojunctionIsSaid`: 79 s (75 before). With its walls graded, the toy disk alone took 146 s, so a sanitized build gives the disk even columns.
  - test_fem_laser `aToyLaserAsItsRateEquations` 77 s, `itsCoarseHeterojunctionsSaidOnce` 51 s.
