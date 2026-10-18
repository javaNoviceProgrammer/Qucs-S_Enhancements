# Coupled RF lines: the reviewer's wishlist of 18 October

The reviewer's round (`2026-10-18-claude-coupled-line-rf-wishlist.md`) solved a ground-signal-signal-ground (GSSG) line: gold 1 µm thick on 500 µm of silica, signals 10 µm wide and 20 µm apart, 5 µm from the grounds. They found its even and odd modes two ways that agree:
- the full model's matrices, reduced to the grounds by hand;
- two half-models, each with the single-line Line Parameters.

They listed six items. Five are done whole and the first in all but its symmetry option (see Not done). The reviewer's files were copied to scratch and run there through the build's tools.

## 1. Coupled-line Line Parameters

**Input.** A Line Parameters node takes several signal terminals, split by commas: `"terminal": "1, 2"` (es), with as many coils in their order, `"conductor": "1, 2"` (mf), and the `"return"` coil as before. With one terminal it is the single line, as it was.

**Matrices, per metre:**
- **C**, Maxwell's (C_ij the charge on i of j's volt). Each terminal is put at 1 V in turn, the others and the grounds at 0, with one factorization.
- **C0**, the same with every εr 1. It is solved once and kept with the solution, not again at each frequency.
- **G**, the dielectrics' losses: ω ∫ ∇φ_i·ε0 εr tan δ ∇φ_j + ∫ ∇φ_i·σ ∇φ_j, from each terminal's own field (tensors where the dielectric is anisotropic).
- **R and L**, the coils' impedance matrix reduced against the return: Z_ij - Z_ir - Z_rj + Z_rr.
- **Quasi-TEM** (no magnetic fields): L = C0⁻¹/c², R and G none.

**Modes:**
- **A symmetric pair** (its diagonals equal to 1 %, the mesh's asymmetry averaged out): the even and odd modes. Each has Z0 (complex), n_eff = β/k0, α (dB/m), β, epsilon_eff = (β/k0)², epsilon_eff_qs (its C/C0) and v. Also Zdiff = 2 Z0_odd and Zcm = Z0_even/2.
- **Other lines** (asymmetric, or more than two): the modes of (R + jωL)(G + jωC), numbered from 1 by n_eff, the highest first. Each has the same values, plus its voltages on the conductors (the largest 1, `V_<mode>_<line>`) and currents (Y v/γ, `I_<mode>_<line>`). The characteristic impedance matrix Zc = Tv γ⁻¹ Tv⁻¹ Z is given too (`Zc_<i>_<j>`).
- A mode's impedance is vᵀv / vᵀi. For a symmetric pair it is the even or odd Z0 of one line.

**The reviewer's pair, one node** (`"terminal": "1, 2"`, `"conductor": "1, 2"`, `"return": "3"`) on their full model, against their table:

| Mode | f | L (nH/m) | R (Ω/m) | Z0 (Ω) | n_eff | α (dB/cm) | Their table |
|---|---|---|---|---|---|---|---|
| odd | 1 GHz | 432.0 | 2642 | 84.72 - j34.42 | 1.831 | 1.354 | 432.0, 2642, 84.7 - j34.4, 1.83, 1.35 |
| odd | 10 GHz | 390.1 | 3827 | 73.79 - j5.72 | 1.595 | 2.254 | 390.1, 3827, 73.8 - j5.7, 1.60, 2.25 |
| odd | 50 GHz | 373.8 | 6443 | 72.04 - j1.97 | 1.557 | 3.890 | 373.8, 6443, 72.0 - j2.0, 1.56, 3.9 |
| even | 1 GHz | 682.8 | 3264 | 126.2 - j42.53 | 1.831 | 1.124 | 682.9, 3264, 126.2 - j42.5, 1.83, 1.12 |
| even | 10 GHz | 585.3 | 5445 | 110.3 - j8.12 | 1.600 | 2.146 | 585.3, 5445, 110.3 - j8.1, 1.60, 2.15 |
| even | 50 GHz | 558.0 | 9521 | 107.4 - j2.91 | 1.559 | 3.855 | 558.0, 9521, 107.4 - j2.9, 1.56, 3.85 |

Quasi-static εeff: even 2.301, odd 2.298 (theirs 2.302, 2.298). Its quasi-TEM L (from C0): 528.9 and 354.7 nH/m, their L_TEM. The run (the app's start, the solve and the node at three frequencies) took 3.5 s.

**Exports:**
- **Touchstone, a 2N-port.** Ports 1 to N are the lines' near ends, N+1 to 2N their far ends (said in the file). Its Y-matrix comes from the modes: Y11 = Ti coth(γℓ) Tv⁻¹, Y12 = -Ti csch(γℓ) Tv⁻¹, with no overflow for a long lossy line. S is for ports of the reference impedance.
- **Mixed-mode, a pair's** (`"mixed": true`): ports d1, d2, c1, c2, the differential and common modes at each end ([Sdd Sdc; Scd Scc]), their references 2 zref and zref/2 said in the file.
- **SPICE, sections.** Each line has its R and L in series. Mutual L is by K, and mutual R by current-controlled voltage sources (H) of the other lines' currents, each sensed by a 0 V source. C and G go to ref and between the lines.

  Checked in ngspice 46 (AC) against the 4-port, the reviewer's pair 10 mm long at 10 GHz with 400 sections:

  | | Sections | The 4-port |
  |---|---|---|
  | S11 | 0.1454 + j0.0529 | 0.1458 + j0.0520 |
  | S21 | 0.04745 + j0.02486 | 0.04745 + j0.02489 |
  | S31 | -0.72142 + j0.16059 | -0.72142 + j0.16059 |
  | S41 | 0.019126 + j0.008681 | 0.019125 + j0.008681 |

  With 50 sections, S11 is 0.1428 + j0.0596.
- **A Qucs subcircuit, a symmetric pair's:** a coupled line part (CTLIN) of the even and odd modes. Ze and Zo are their Z0's real parts, Ere and Ero their (β/k0)², Ae and Ao their attenuation in dB per metre (the part's dB is a power ratio per metre). Ports 1 and 2 are the near ends, 3 and 4 the far ends. Copy Part copies it.

  Checked in qucsator at 10 GHz: S31 is -0.7232 + j0.1689 against the 4-port's -0.7214 + j0.1606. The difference is the Z0s' imaginary parts, which the part has no place for (said in its note).
- An LTRA line is one line: asked for coupled lines, that is said.
- After a solve, the default plots over frequency show each mode's Z0 and α.

## 2. Conductor roles in magnetic fields

**A single conductor's coil has a "role":**
- **Fixed current**, the default, as before: a port of the coils' matrix.
- **Fixed voltage**, `V0` between its ends (over the model's thickness; round the axis, one turn); its current is found.
- **Floating:** no net current, its voltage found.
- **Grounded:** no voltage between its ends, its current found (mf.I_<name>, and _im). This is what a conductor with no coil was, now said and with its current.

The coils' matrix is that of the coils fed a current. The others are held as their roles say during the sweep, a voltage at none.

**The physics' "Reference coil"** (`"reference"`): the matrix reduced against it, mf.Lr_i_j and mf.Rr_i_j = Z_ij - Z_ir - Z_rj + Z_rr. It must be a coil fed a current, or that is said.

**The pitfall** is now in the coil's and the return's tooltips: an odd mode's half-model's ground is grounded, not fed -1 A. On the reviewer's odd half-model with its ground a Grounded coil, the line is the same as with no coil (373.8 nH/m at 50 GHz). Its ground carries -0.428 - j0.011 A, so the rest of the return flows in the mirror signal. Their coupled model with `"reference": "3"`: Lr_1_1 ± Lr_1_2 at 50 GHz is 558.0 and 373.8 nH/m, the even and odd modes', with no reduction by hand.

## 3. Two effective permittivities

A single line of method R, L, G, C now also gives:
- `n_eff` = β/k0;
- `epsilon_eff_qs` = C/C0, the dielectric's alone (C0 with every εr 1, solved once).

`epsilon_eff` stays (β/k0)², n_eff's square. The node's note says which is which: the conductors' loss slows the wave in epsilon_eff. On the reviewer's odd half-model, epsilon_eff is 3.353, 2.544 and 2.424 at 1, 10 and 50 GHz; epsilon_eff_qs is 2.299 at each. Quasi-TEM gives n_eff too. Coupled lines give both for each mode.

## 4. Units of R

R per metre is shown in Ω/m (2.642 kΩ/m) where it was m·kg/(s³·A²). The SPICE files' comments use it too.

## 5. Paths when making a model

`fem_model action=new` with a relative path:
- **Missing folders are made**, as `import` already did, and that is said.
- **A write that fails** (a folder that cannot be written) closes the tab it made: nothing is left open in that file's name. A folder that cannot be made is said before any tab is made.
- **A path whose first folder is one of the workspace's but not the open project's** (the reviewer's `gssg_rf/` with `fan1998_phc_cdf` open): it still goes to the project first, as every relative path does. The note says where it went and gives the workspace's path.

## 6. fem_describe what=format group=…

With a group, the format's text is cut down to the paragraphs that name the group, its kinds or their physics' tags. The library's materials are left out unless the group is material. The note says so and how to have it all: `"text": "all"`, or `"none"` for no text.

| | Characters returned |
|---|---|
| Whole | 242,169 |
| `group` mf | 8,817 |
| `group` derived | 24,017 |
| `group` es | 18,439 |

The format's own text now describes the coils' roles, the reference coil, coupled lines and their exports.

## Not done

- **The symmetry option** (a half-model made and solved automatically). The two halves need different conditions on the wall for each physics: es ground and mf A = 0 for odd; es zero charge and mf PMC for even. They also need a mirror of the geometry, its selections and its mesh. As two solves it would save little over the full model: the reviewer's full model took 5 s. The half-models stay a user's choice, now with the Grounded role for the odd one's ground and the full model's matrices for comparison.
- **ngspice's CPL** (its coupled lossy line) was written and tried, then left out:
  - It runs in transient analysis only: in AC it stops with "matrix is singular", ngspice's own manual example too.
  - In transient its result depends on the time step. A pair of R 300 Ω/m, 10 mm long, driven through 50 Ω: at 1 ns its near end is at 0.63 V, and at 0.78 V with a step five times smaller, where the line settles to 0.515 V.
  - With a slow edge it settles to 0.502 V, as though R were not there.
  - One line through it is worse: near and far ends together above the source's 1 V.
  - ngspice's own manual example (R 1 Ω/m) agrees with sections. This also showed that its matrices are written in the right order (each upper triangle, row by row).

  It would mislead, so the sections are the SPICE form of coupled lines. LTRA stays for one line.
- **Full-wave RF modes**: the reviewer's own not-done, unchanged.

## Tests

- **test_fem_magnetics `aCoupledPair` (new).** A smaller GSSG pair (gold 5 µm thick, signals 20 µm wide, 30 µm apart) at 1 and 20 GHz:
  - C is the terminal sweep's;
  - R and L are the coils' matrix reduced by hand, and the physics' own Lr and Rr against its reference coil;
  - the even and odd modes are those of the averaged matrices to 1e-9, with εeff_qs their C/C0 and (β/k0)² above it;
  - the derived values are there, R in Ω/m, Zdiff and Zcm;
  - quasi-TEM: L = C0⁻¹/c²;
  - the odd mode is the half-model's with its ground grounded, to 4e-5 in Z and n_eff (2e-3 allowed), its ground's current part of the return;
  - one line through the coupled path equals lineRLGC (G by each terminal's field and by the solution's, to 1e-6);
  - the 4-port is reciprocal and lossy, its mixed-mode Sdd and Scc are the odd and even modes' lines and Sdc is none;
  - the files: Touchstone rows and ports, mixed-mode, SPICE sections (K, H, C between), the CTLIN part; an LTRA of coupled lines is refused.
- **test_fem_magnetics `anAsymmetricPair` (new).** Signals of 20 and 40 µm: modes 1 and 2 by n_eff, Z i = γ v and Y v = γ i for each, Zc Y Zc = Z, their vectors and Zc among the values. There is no Qucs part, and fewer coils than terminals is said.
- **test_fem_magnetics `coilRoles` (new).** Two wires at 10 kHz, the model 0.5 m thick:
  - grounded is the same as no coil, with its current;
  - the matrix is of the coils fed a current alone;
  - fed a voltage of the current-fed coil's, the coil carries 1 A with the same R and L;
  - floating carries no current;
  - Lr and Rr are the loop's, and V1 - V2 at ±1 A is the loop's too;
  - a reference that is not fed a current is refused;
  - at rest a voltage drives V/R, and grounded carries none.
- **test_fem_magnetics `coaxLineParameters`:** R in Ω/m; n_eff = β/k0, epsilon_eff its square, epsilon_eff_qs the PTFE's 2.1 and less than (β/k0)².
- **test_multiphysics_doc `aLineOverFrequency`:** twelve values at each frequency, R in Ω/m.
- **test_multiphysics_doc `claudesNewModelsFolders` (new).** A folder made; the workspace's folder name with a project open said; a folder that cannot be written leaves no tab; a folder where a file is cannot be made.
- **test_multiphysics_doc `claudesFormatOfAGroup` (new).** mf's text is its paragraph, under a third of the whole and a quarter of the reply; no materials; derived names lineparams; `text` all and none; materials with group material.

## Breaks

Twenty-five changes broken on purpose, all caught:
- **Coupled lines:**
  - R and L not reduced against the return;
  - G not times ω;
  - the odd mode as the even;
  - a mode's currents not over γ;
  - Zc without γ⁻¹;
  - Y12's sign;
  - the mixed-mode d and c swapped;
  - no symmetric pair found;
  - C0 not in vacuum;
  - an LTRA of coupled lines let through.
- **A single line:** εeff_qs's C0 not per metre (caught by the test model's 2 m thickness), and no Ω/m.
- **Coil roles:**
  - roles not read;
  - a floating coil held at none;
  - a held voltage not a source;
  - the matrix of every coil;
  - the reduction's Z_rr sign;
  - at rest a voltage not over the length (caught by the 0.5 m thickness);
  - a reference coil not checked.
- **The tools:** a group's whole text; no folder made; a tab left after a failed write; the workspace's folder name not said.
- **The window:** the default graphs of one Z0, and Copy Part an RLCG part (refused: the terminals at 1 V both).

## Runs

- **Release:** the suite 129 of 129; the new functions 0.7 s (`aCoupledPair`), 1 s or less each.
- **Under the sanitizers, with Eigen's LU (as on CI's Linux):** no report. `aCoupledPair` 18 s (a coarser mesh and 20 GHz alone there), `anAsymmetricPair` 11 s, `coilRoles` 21 s, `coaxLineParameters` 19 s, `aCoupledPairsGraphsAndPart` 13 s, `aLineOverFrequency` 46 s.
- **The lints pass:** JSON nesting, tool arguments, third-party names.
- **Checked by hand:** the reviewer's three models through the tools; the SPICE sections in ngspice 46 (AC); the CTLIN part in qucsator; ngspice's CPL (left out, above).
