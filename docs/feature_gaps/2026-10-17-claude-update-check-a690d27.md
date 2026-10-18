# Update check 2026-10-17 (a690d27)

The reviewer's check of 2522d21..a690d27 (`2026-10-17-claude-update-check-a690d27.md`) passed its five items, and the sixth (a690d27's layout tests) was not checked. It left two notes, both taken up here.

The reviewer's files in `~/QucsWorkspace/update_check_1017c` (toy/, liu2013_lateral_implant.qfem, vcsel_laser.qfem, vcsel_c40.qfem) were copied to scratch and solved by this build's app through its tools.

## 1. A coarse mesh at a heterojunction, said

**The note.** The toy laser at 1.7 V on 4 rows across each layer passes 3.5 times too little current above threshold, and nothing in the log says so. The reviewer suggested a hint when a heterojunction is crossed by few rows.

**What sets it.** Reproduced: 10.32 µA at 4 rows, against 36.6 at 256. Along the axis through the active layer at 1.7 V, the claddings beside the heterojunctions are depleted over 10 to 30 nm, their carriers changing by orders of magnitude within a few nanometres. On 4 rows a cladding's row is 75 nm (the n side) or 120 nm (the p side): the whole layer inside one element.

The scale is the Debye length, of the doping or the carriers, whichever is more: 4.1 nm in the claddings (1e18 per cm³), 1.1 to 1.5 nm in the active layer at 1.7 V. The reviewer's convergence follows it:

| Rows | Claddings' rows | Current at 1.7 V | Said |
|---|---|---|---|
| 4 | 75, 120 nm | 10.32 µA (28 %) | yes |
| 16 | 18.7, 30 nm | 16.29 µA (45 %) | yes |
| 64 | 4.7, 7.5 nm | 32.59 µA (89 %) | yes |
| 128 | 2.3, 3.8 nm | 35.6 µA (97 %) | no |
| 256 | 1.2, 1.9 nm | 36.6 µA | no |

**Now.** After each solved bias point that carries current, the solver looks at the edges leaving each heterojunction node that carry the current across it (`SemiconductorProblem::heterojunctionMesh`). An edge is counted when:
- **its share of the contacts' current** is at least a tenth of the most. The share is the edge's current times its contact weight's change across it; summed over all edges, these shares give the terminal current, so it measures what an edge carries from contact to contact. The lateral edges in the active layer by its side walls, where the carriers run along the walls, have no share, though they carry the most current of any;
- **it runs across the interface,** more than 60° off it, and not from a corner where two interfaces meet. From the disk's corner, a 50 nm edge along the top interface's line was otherwise named, and refining it changes nothing.

Where such an edge is longer than the Debye length at its far node, a warning names the length, the domain and the Debye length:

> Warning: Semiconductor: elements 120 nm across where its current crosses a heterojunction (in "pclad"), its Debye length there 4 nm (its carriers' or doping's): the current depends on the mesh - refine it there to under a Debye length (a Layered Region's rows, a smaller size near the interface) until the current no longer changes

A study gets one such line, its worst point's: a Laser L-I step replaces it as points get worse. The check is skipped at rest (no current), graded over an element (one side's bands throughout), and on a line solved in 1D.

**Not said:** the VCSEL example (10 and 40 cavity rows, the same current to 0.9 %), the Ge/Si electroabsorption modulator and the Ge photodiode (heterojunctions, meshed fine enough), and the Liu model (no heterojunction).

**Found on the way, not said by it.** The toy's current also depended on the lateral element size: at 128 rows, 35.6, 41.8 and 57.8 µA at 50, 100 and 200 nm laterally. It was put down here to the current funnelling into the 0.3 µm disk. The cause was the disk's side-wall nodes, each holding half a column of the cladding at the disk's carriers. Fixed afterwards with graded columns at such walls (`2026-10-18-claude-lateral-columns-at-walls.md`): 29.8, 29.6 and 29.5 µA.

## 2. A contact's charge, said as it is

**The note.** `semi.Q_a` and `Q_k` in the Liu model are constant at every bias (273.7 and −501.6 fC) and do not sum to nought. That is by design, but the name invites misreading, and the description said "about nought".

**What it is.** An ohmic contact's charge is the field's flux into the material at the contact. Both of the Liu model's contacts sit on implanted regions whose doping changes steeply with depth at the surface. That gives a built-in field at each contact, and a charge on its metal that the bias does not change. The junction's own charge is balanced inside the semiconductor, and the contacts' charges sum to minus the semiconductor's net charge, not to nought. The contacts' values are neutral with incomplete ionization (`neutral()`), so this is not the boundary condition's doing.

**Now.** The description says so:

> charge on a's metal, the field's flux at it - not the junction's charge, which is balanced inside the semiconductor: nought on uniform neutral material, a constant where the doping changes at the contact (an implant's slope); the junction's capacitance is the small-signal semi.Caa

The name is kept: renaming it would break models and scripts that read `semi.Q_<contact>`.

## Tests

- **test_fem_semiconductor `aCoarseMeshAtAHeterojunctionIsSaid` (new).**
  - The double heterostructure strip at 1.6 V on 4 rows: said once, "elements 120 nm across … (in "pclad") … 4 nm".
  - Not said on 128 rows, at rest, or graded over an element.
  - A 100 nm GaAs stripe in Al0.6Ga0.4As, its side walls 50 nm elements across, on 128 rows: not said.
  - The stripe swept to 1.6 V on 4 rows: a line at most per point, the last the worst.
  - The toy's disk at 1.7 V on 16 rows (no laser): a cladding's row named, not the 50 nm corner edge.
  - A sanitized build skips the two 128-row cases: 75 s under the sanitizers here.
- **test_fem_laser `itsCoarseHeterojunctionsSaidOnce` (new).** The pillbox laser with abrupt heterojunctions, an L-I of three points: one line, "elements 123 nm across".
- **test_fem_semiconductor `contactCurrents`.** The charge's description, and the charge itself under 1 % of C V on the uniform junction.

## Breaks

Six changes broken on purpose, all caught:
- the check not called;
- every crossing counted, whatever its share (the stripe's side walls said);
- every direction counted (the toy's corner edge named);
- a line a point (two lines in the L-I);
- said only past 100 Debye lengths;
- the description back to "about nought".
