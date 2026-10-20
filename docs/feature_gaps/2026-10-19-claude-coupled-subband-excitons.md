# Coupled subband excitons for multi-quantum-well spectra

The user (2026-10-19): *"Looks like qucs doesn't have a coupled subband exciton feature for multi quantum well simulations. Can you add it?"*

## What there was

The spectrum step's `"excitons": true` gave each electron–hole subband pair its own 1s exciton, a variational one: two parameters (λ, γ), or Bastard's one (λ). The continuum above each pair's edge was raised by the ideal 2D Sommerfeld factor scaled to that binding.

No pair felt another. So the model could not show:
- a wide well's exciton drawing on its higher subbands;
- the 2s and higher states;
- a real well's own continuum enhancement;
- a dark (indirect) pair's exciton mixing with a bright one;
- a bound state's Fano line in another pair's continuum.

## What it does now

`"excitonmodel": "coupled"`: in the panel, Exciton → *Coupled subbands (k space)*. It is the momentum-space exciton of Chuang, Schmitt-Rink, Miller and Chemla (1991), with its subbands coupled as Winkler (1995) did. New: `multiphysics/fem_exciton.h/.cpp`.

**The Hamiltonian.** Every pair ν = (eᵢ, hⱼ) sits on one in-plane k grid, in the s channel (the light sees no other):

> E_ν(k) φ_ν(k) − Σ_ν′ ∫ d²k′/(2π)² V_νν′(|k − k′|) φ_ν′(k′) = E φ_ν(k)

with

> V_νν′(q) = e²/(2ε q) F_νν′(q), and F_νν′(q) = ∫∫ ρe_νν′(zₑ) ρh_νν′(zₕ) e^(−q|zₑ − zₕ|) dzₑ dzₕ.

**The numerics:**
- **The grid** is Gauss–Legendre panels in energy. They run from Ry/24 apart near the edge, growing by a quarter per panel, to the continuum's spacing (half the line width, 0.5 to 3 meV). Past the continuum's reach, 40 more points cover k = kmax/u, u ∈ (0, 1], out to k infinite. Without them a bound state's short range is lost: the ideal 1s came out 3.65 Ry of 4, its line half as strong.
- **The Coulomb kernel's s channel** is (2/π)K(m)/(k + k′), its elliptic integral by the arithmetic–geometric mean. Its log singularity is removed by subtraction (h(x) = 2/(1 + x²); its integral to infinity by adaptive Gauss–Kronrod).
- **Finite thickness** is the regular part (F − F(0))/q. It is tabulated on 240 values of q and averaged over the angle by 24 Gauss points. Each F comes from two exponential sweeps along the stack, O(n).
- **The solve:** one dense diagonalisation gives every state. Each state's strength per component is |Σ A_ν(k) √W u|², the pairs' amplitudes summed coherently.
- **The continuum's lines:** a state above the edge is spread over its own cell, the energies of its largest part's share of the plane, kₕ² − kₗ² = 4πW, moved by its Coulomb shift. Without the attraction that reproduces the free spectrum exactly. Past the grid's top, the free continuum's tail stands for the rest.

**The two paths:**
- **Parabolic bands:** heavy and light holes are two separate solves, since the attraction does not couple their orthogonal Bloch functions. Each solve takes the lowest 12 pairs, dark ones too: a pair's overlap may be 0 (parity, or a barrier) and its exciton still mixes. A pair's amplitudes, TE and TM, are its signed overlap times √(Ep · weight/n).
- **k·p bands (4x4, 6x6):** one solve of the lowest 16 pairs. Each pair is an electron state with a valence state. (Since the check of this commit, the heavy and light holes' mixing is in the attraction too: the spinors at each k, in channels of the exciton's J_z - [its write-up](2026-10-19-claude-coupled-exciton-hh-lh-mixing.md). What follows is this commit's.)
  - Their energies and polarization weights along k are averaged over the plane's directions (the axial approximation).
  - The form factors are of the electron's envelope and the valence spinor's at k = 0.
  - The amplitudes are the k = 0 overlaps with X, Y and Z of each spin, coherent between pairs, scaled along k by each polarization's weight. A pair dark at k = 0 in a polarization gets an incoherent part of its own.
  - The 8x8 bands are not done; they keep their existing warning.

**The globals:**
- `qw.Eb` is the e1-hh1 edge less the strongest TE line below it, and `qw.Ex` is that line.
- `qw.aX` is Bastard's λ of that line's |ψ(0)|² (2/(πλ²)).
- `qw.gX` is not defined for this model.

## How it was checked

| Check | Result |
|---|---|
| The ideal 2D exciton (form factor 1): 1s, 2s, \|ψ(0)\|² | 4.0002 Ry (4), 0.4488 Ry (4/9: 1 %), 0.9999 of 8/(πa₀²) |
| Its continuum from Ry to 4 Ry, against the 2D Sommerfeld factor | within 5 % |
| Without the attraction (εr 10⁶): the solver's continuum | the free density to 1 % |
| A thin layer (0.02 nm Gaussians); two 10 nm apart | 3.96 Ry (first order: 4 − 0.06); the indirect 0.98 Ry, its line 6 % |
| A spectrum without the attraction against the one without excitons | parabolic (2 states, 1 meV Lorentzian) 0.00 %; k·p 0.04 % |
| One pair (10 nm infinite well) against a real-space solution of the same exciton (radial finite volumes, 0.01 nm, Sturm bisection) | 8.4470 against 8.4451 meV; \|ψ(0)\|² 3.2865e15 against 3.28688e15 /m² |
| Khramtsov et al.'s exact GaAs/Al0.3Ga0.7As heavy-hole binding, 10 nm | one pair 8.230; 2 states 8.289; 4 states 8.326; exact 8.362 meV |
| 14 nm | 6 states 7.663; exact 7.666 meV |
| 20 nm | one pair 6.657; 2 states 6.785; 4 states 6.894; exact 6.911 meV |
| k·p 4x4, 10 nm infinite well | 8.895 meV (two-parameter variational on the same bands 8.696; parabolic coupled 8.447) |

**What the line's strength showed.**
- **The variational lines are too strong for one pair.** On the same 10 nm well, Bastard's variational 1s has |ψ(0)|² = 4.01e15 /m². That is 22 % above the true one-pair value, while its binding is only 1.5 % low: a variational state's energy is right to the second order, its line only to the first.
- **The coupled line grows with the subbands.** That is the electron and hole correlated along the axis.
  - At 10 nm, |ψ(0)|² is 1.14 times the one pair's with 4 states; at 20 nm, 1.44 times.
  - At 14 nm the radiative decay ħΓ₀ = ∫α dE · L/(2π) comes to 23.0 µeV with one pair, 27.8 with 2 states, 29.7 with 4 and 30.2 with 6. Khramtsov et al. measured 36.6.
  - The two-parameter variational model gives 36.1; the coupled model levels off near 30.
  - I have not tracked down which side the remaining gap is on: the experiment, or z-correlation the subbands do not reach.
  - *(Answered on 2026-10-20: the z-correlation. The products of subbands converge on it slowly; a real-space solve drawing the two together finds the line 1.43 times one pair's in a 9.4 nm well. The coupled excitons now draw the electron and hole together along the axis: 32.6 µeV at 14 nm with one pair. See [the coupled excitons' TE line](2026-10-20-claude-coupled-exciton-strength.md).)*

**Speed.** In Release a spectrum's coupled solve takes about 1 to 3 s: hundreds to 1,200 unknowns, dense. The cost is the eigenvector accumulation, about 100 times slower in the sanitizers' Debug build. So under the sanitizers the tests use wider lines and fewer states, and skip the k·p cases. A solve whose size would pass four times the cap is refused with an error, not attempted.

## Tests

`test_fem_exciton` (new; ctest timeout 1800 s):
- `theEllipticIntegral`;
- `theIdeal2DExciton`;
- `withoutTheAttraction`;
- `envelopesFormFactors`;
- `twoPairsCoupled`;
- `theFreeLimit`;
- `onePairAgainstRealSpace`;
- `subbandsAgainstKhramtsov` (4 states and the 16 heavy-hole pairs, dark ones counted);
- `kpBandsCoupled`.

Twelve changes were broken on purpose:

| Break | Caught by |
|---|---|
| B1 the subtraction's sum left out | theIdeal2DExciton (1s 17.6 Ry) |
| B2 no tail to k infinite | theIdeal2DExciton |
| B3 the pairs not coupled | twoPairsCoupled |
| B4 the amplitudes' signs lost | subbandsAgainstKhramtsov (the line weaker, not stronger) |
| B5 one spin pair (parabolic) | theFreeLimit (50 %) |
| B6 the k·p TE weight | theFreeLimit |
| B7 no cells: the continuum as lines | theFreeLimit (46 %, after the test's lines were made narrow and Lorentzian) |
| B8 dark pairs left out | subbandsAgainstKhramtsov (its pair count) |
| B9 no thickness within a pair | envelopesFormFactors |
| B10 3 angular points, not 24 | not caught: harmless. The regular part's angular average converges at once (one pair 0.011 %, the indirect 0.07 %) |
| B11 the k·p amplitude not scaled along k | theFreeLimit |
| B12 no free continuum past the grid | theFreeLimit (6.4 %) |

## Runs

- **Release:** `test_fem_exciton` 11 of 11 in 17 s, and 3.7 s on the lighter path the sanitizers take.
- **Sanitizers** (ASan and UBSan, Eigen's LU): `test_fem_exciton` 10 passed, the k·p one skipped, no report, 303 s in all. `subbandsAgainstKhramtsov` takes 232 s of that (20 nm alone).
- **The Release suite:** 131 of 131.
