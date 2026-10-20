# Coupled excitons: the heavy and light holes mixed in the attraction (the check of ac8ce07)

The reviewer's file: `~/Desktop/qucs-reviews/2026-10-19-claude-coupled-exciton-hh-lh-mixing.md`. It checked the hh/lh mixing of `"excitonmodel": "coupled"` (ac8ce07; [its write-up](2026-10-19-claude-coupled-subband-excitons.md)) on the reviewer's well, `gaas10_kp4.qfem`: 10 nm GaAs between 25 nm Al0.3Ga0.7As, 4x4 bands, a 0.5 meV Lorentzian, 4 states.

**The verdict:** the mixing was only partial. It reached the exciton through the k·p bands' dispersions and polarization weights, never through the Coulomb coupling. The hole densities depended on the eigenvectors' phases, and the hh1 exciton had a TM line it should not have. Two smaller issues: the continuum widened without a warning, and the log counted 0 transitions.

Each item was reproduced first on the reviewer's model, over `qucs-s --mcp-server` with an isolated home and cache: hh1 bound by 9.058 meV, TM/TE 0.181 at its line, the continuum's states 98.9 meV apart (a log line), "a spectrum of 0 transitions".

## The items

| Feedback | Now |
|---|---|
| **1. No hh–lh Coulomb mixing.** The k·p path built each pair's form factors from the valence spinor at k = 0. There a heavy and a light hole's Bloch parts are orthogonal, so every coupling between them was zero: two separate solves in one block. Winkler's treatment needs the spinors at each k in the kernel, each J_z component in its own angular channel. | **Done as asked, in the axial approximation.** The excitons are solved in channels of their angular momentum about the axis, J. A valence state's spinor at k has components e+↑ (J_z 3/2), Z↑ (1/2), e-↑ (-1/2), e+↓ (1/2), Z↓ (-1/2), e-↓ (-3/2); at the angle θ each is e^(-iμθ) times its own at 0. So two pairs attract, in channel J, through each component's Coulomb channel \|J - μ\|, with their spinors at their own k: the heavy and light holes, mixing along k, are coupled. The light sees J 3/2 (TE by e+↑) and 1/2 (TE by e+↓, TM by Z↑); -J is the same by time reversal, so each is solved once and counted twice (checked: J -3/2 and -1/2 give the same states to 9 digits and the same strengths to 1e-8). **The kernel's channels:** the 2D Coulomb kernel's channel L is Legendre's Q_{L-1/2}((k² + k'²)/2kk')/(π√(kk')). Near k' = k it comes from K and E and the recurrence, far off from its hypergeometric series. Its excess over the s channel is regular and is added at k' = k too. The regular part (finite thickness) takes cos(Lθ) in its angular mean. **The spinors:** the bands' at each of their k points, turned into the circular components above, are expanded on a basis per J_z: the principal components of each J_z's parts at k points 0, 1, 2, 4, 8 ... and the last, holding all but 1e-5, 12 at most. That is 48 functions here, holding 98.75 % of every state at least, to the bands' top (2.66 nm⁻¹). Between the bands' k points the coefficients are cubic. **Its effect** on the reviewer's well: hh1 bound by 8.64 meV, not 9.06 (8.66 with the mixing alone, the rest the shorter reach below). The light-hole components the heavy hole gains along k sit in p and d channels, which attract less. The lh1 exciton, in channel 1/2, takes in the heavy holes' parts: its line 0.4 meV higher, its TE peak 21 % lower, its TM peak 28 % higher. The help now says the mixing is in the attraction as well as the bands. |
| **2. `.real()` of the hole density: gauge-dependent.** Between two valence states the product is complex and carries their relative phase. The eigensolver's phases are arbitrary, so the kernel matched no single choice of phases. | **The kernel is complex Hermitian, its densities whole.** Two pairs couple by c_a(i)† M c_b(j), the holes' coefficients at their k on either side, so any phase of a state cancels against its amplitude's. **Real where the bands allow:** in phases e± by -i on spin up, Z by -i on spin down, the k·p bands along x are real (their X-Z terms imaginary, spin and orbit real). So each degenerate set is made real (its vectors' real and imaginary parts' span), lined up with the k before's (the nearest unitary to their overlaps), and the Hamiltonian is solved real, 2.2 times as fast. Bands that are not real so (direction-averaged, or warped) give a complex solve, said in the log. **Checked:** the reviewer's well solved complex (as it was before the deepest states were left out, below) and real: the same states, 8.657 meV both. |
| **3. A spurious TM line on hh1, 0.18 of TE.** A pair dark at k = 0 got an incoherent "part of its own" along k. A bound s-exciton's lh admixture is p-like about it, so its TM integral vanishes by the angle. | **Gone with the parts.** In channel J 3/2 there is no TM component, and in 1/2 the heavy-hole pairs are p and d. The hh1 1s has no TM: **0.0035 of TE at its line**, the lh line's Lorentzian tail (the parabolic bands' 0.003). Each pair's channel amplitudes are its spinor's overlaps, scaled to its own weights at the bands' k points, linear in k² between them as the bands' are, so the free continuum is the spectrum's without excitons (0.03 %). (Not to the bands' states' weights: where states are one energy, as the heavy and light holes without mixing, those are any mixture of them, and the TE/TM split came out 48 % off.) |
| **4.1 The spacing widened without a warning.** | **A warning, once, naming what helps.** "the coupled excitons' continuum states 39.7 meV apart, not the 0.5 the line's width asks (16 pairs on 72 k points: its 1200 unknowns, "excitonsize"): above the edges, where pairs' continua mix, its absorption ripples the more the narrower the line - a line of 1 meV or more smooths it (...: some 7 % at 0.5 meV, 2 % at 1, under 1 % at 2)". **The size is the step's `"excitonsize"`** (100 to 20000, 1200 by default), in the panel under the exciton model. **What helps was measured, and is not the size:** at 2400 unknowns the states are 5.4 meV apart and the ripple is the same. It comes from states mixing two pairs' continua: their strengths vary from state to state, and a 0.5 meV line cannot average over several. Two causes found and fixed on the way (below) brought it from ±13.6 % to ±7 %. |
| **4.2 "a spectrum of 0 transitions".** | It counted the parabolic bands' transitions, none with k·p bands (with or without excitons). **Now** "a spectrum of 64 transitions (16 of them the coupled excitons')". |
| **4.3 The k·p test did not reach the mixing.** | The three tests asked for, and more (below). |

## Found on the way

| | |
|---|---|
| **The continuum's reach took 30 kT** - 0.78 eV at 300 K, for carriers. Excitons have none (carriers screen them out). | The reach is the window's top and 30 line widths past it, 0.3 eV at least. The same number of unknowns now covers 0.3 eV, not 0.78: the states 2.5 times closer. Khramtsov et al.'s 10 nm binding moved from 8.326 to 8.322 meV with 4 states (exact 8.362). |
| **A continuum state was spread over its largest part's cell alone.** Where pairs' continua overlap (e1-hh1 with the dark e1-hh2, e1-lh1), a state is a mixture, and its strength went over one pair's cell. | Over its largest parts' cells, those holding 90 % of it (8 at most, each 1 % at least), weighted by their shares. Without the attraction it is the same: each state is one part. The ripple at 1 meV went from 3.4 % to 2.3 % (TE) and 2.4 % to 1.4 % (TM), parabolic; at 0.5 meV, k·p, ±10 % to ±7 %. |
| **The bands' solve can miss a Kramers partner** of its deepest states (states 14 and 15 at one k, 74 µeV apart, each the other's missing partner). | The spinors are taken to the deepest state used and its partner, so the real phases hold. |
| **The form factors' tables took 6-7 s a channel**, each hole density's convolution with e^(-q\|z\|) swept at every q. | Each electron density's convolution once, the holes' densities against it in one product: 53 ms. |

## The reviewer's well, before and after

| | ac8ce07 | now |
|---|---|---|
| hh1 exciton | bound by 9.058 meV, at 1.4521 eV | bound by 8.635 meV, at 1.4525 eV |
| TM/TE at its line | 0.181 | 0.0035 |
| lh1 line (TE, TM peaks) | 1.4648 eV (5.4e6, 10.7e6 /m) | 1.4652 eV (4.3e6, 13.7e6 /m) |
| log | "0 transitions"; the spacing a log line | "64 transitions (16 of them the coupled excitons')"; a warning |
| continuum 1.48-1.50 eV, 0.5 meV line (TE, TM) | ±13.6 %, ±13.2 % | ±7.3 %, ±6.7 % |
| the same, 1 meV / 2 meV | - | ±2.2 %, ±2.3 % / ±0.4 %, ±0.6 % |
| solve | 27.0 s | 30.8 s (the k·p bands 23 s of it; each channel's dense solve 2.5 s) |
| parabolic bands: hh1, solve | 7.957 meV, 4.5 s | 7.951 meV, 5.1 s |

(The ripple is the largest departure from the mean over the 9 energies about each, 2 meV, as measured here; the reviewer's measure gave ±18 % and ±19 % before.)

## How it was checked

| Check | Result |
|---|---|
| The Coulomb channels L 0-3 against the angle's integral by Gauss-Legendre | within 1.3e-11; far off (k'/k = 1e4, where the quadrature cancels) its series in k/k' to 1e-12; at k' → k each channel's excess over s (ψ(1/2) - ψ(L + 1/2))/(πk) |
| **A hole of two components mixed along k, against the same exciton in real space.** J_z 3/2 and -1/2 coupled by g k₋² (as Luttinger's R couples them); D 15 meV; masses 0.067, 0.11, 0.21; strictly 2D. The reference: its radial equations with the derivative couplings in them, s and d waves, the energy functional's finite volumes to 0.01 nm and Richardson (with g 0 the ideal 2D values to 1e-6). | g 0.2 eV nm²: 14.11021 against 14.10853 meV, \|ψ1(0)\|² 1.04279e16 against 1.04296e16 /m². g 0.4: 16.04147 against 16.03751, 1.46425e16 against 1.46474e16. The second component in the s channel instead: more than 2 % off. |
| The holes' phases: each band's spinor and amplitude by a phase of their own at every k, a basis state by another | the same 40 lowest states to 1e-10 eV, their strengths to 3e-16 (the Hamiltonian complex) |
| The k·p bands without their mixing (γ2 = γ3 = 0: the heavy and light holes one mass) against the parabolic bands of that mass | bound by 9.6061 meV both; the spectrum within 5e-4 of its peak |
| Without the attraction (εr 1e6): the spectrum without excitons | parabolic 0.00 %, k·p 0.03 % |
| The k·p test well (4x4, 10 nm infinite): bound by | 8.687 meV (8.895 with the spinors at k = 0; two-parameter variational 8.696; parabolic coupled 8.447) |
| -J's channels against J's (one run, the reviewer's well) | the same states to 9 digits, strengths to 1e-8 |

## Tests

`test_fem_exciton`:
- **new:** `theCoulombChannels`, `aMixedHoleAgainstRealSpace`, `anyPhaseTheSame`, `kpWithoutMixingAsParabolic`, `aMixedContinuumSpread`, `theContinuumWidenedSaid`;
- **more in:** `kpBandsCoupled` (no TM at the hh1 line; its binding, 8.687 meV; the log's transitions).

Fourteen changes were broken on purpose, and all fourteen were caught:

| Break | Caught by |
|---|---|
| R1 the Hamiltonian's real part alone | anyPhaseTheSame |
| R2 every component in the s channel | aMixedHoleAgainstRealSpace (8.6 % off), kpBandsCoupled (8.754 meV) |
| R3 the k·p spinors at k = 0, as in ac8ce07 | kpBandsCoupled (8.895 meV) |
| R4 TM by Z↑ in J 3/2 too | theFreeLimit (TM doubled), kpWithoutMixingAsParabolic |
| R5 the amplitudes not scaled to their weights | theFreeLimit (1.2 %) |
| R6 a state's largest part's cell alone | aMixedContinuumSpread (3.4 %, 2.4 %) |
| R7 the transitions counted as before | kpBandsCoupled (the log) |
| R8 no warning | theContinuumWidenedSaid |
| R9 "excitonsize" ignored | theContinuumWidenedSaid |
| R10 the Legendre recurrence's coefficient | theCoulombChannels, aMixedHoleAgainstRealSpace, kpBandsCoupled |
| R11 the spinors not lined up along k | theFreeLimit (12.6 %), kpWithoutMixingAsParabolic (the binding 7.6 % off) |
| R12 each J once (no -J) | theFreeLimit (50 %), kpWithoutMixingAsParabolic |
| R13 e+↓ and Z↓ given each other's J_z | kpWithoutMixingAsParabolic |
| R14 the bands' states' weights, not their own | kpWithoutMixingAsParabolic (48 %) |

## Runs

- **Release:** `test_fem_exciton` 15 of 15 in 25 s (17 s before, for 9); the Release suite 131 of 131, after the breaks' rebuild.
- **Sanitizers** (ASan and UBSan, Eigen's LU): `test_fem_exciton` 12 passed, the 3 of the k·p bands' and mixed continua's dense solves skipped, no report, 152 s (303 s before: the reach without kT, the tables).
- **The scenarios** (`scripts/mcp-e2e-scenarios.py`, the build's app): 97 checks, none failed.
- **The reviewer's model** and its variants as in the table above, over `qucs-s --mcp-server`.

## What it does not do

- **The axial approximation.** The channels need the plane isotropic. With the bands along [100], [110] or averaged over the plane's directions, the first direction's spinors are taken for every direction, said in the log; the energies and weights are still the directions' average.
- **A narrow line's continuum, where pairs' continua mix, ripples**: some 7 % at 0.5 meV, 2 % at 1 meV, under 1 % at 2 meV on the reviewer's well. That is the discrete continuum: its states' strengths vary where continua mix, and a larger size does not help. The warning says so. (These are the largest departures from a 2 meV local mean. The ripple's peaks about its trend are larger, ±11, ±4 and ±1.5 %, and the warning quotes those since the check of this commit: [its write-up](2026-10-20-claude-coupled-exciton-a4ff84a-check.md).)
- **Not tested:** a Kramers pair kept whole at the 16-pair cap (its cut moved down to the pair's start); the reach without kT (a speed and resolution change: the binding moved 0.05 %).
- The 8x8 bands keep their existing warning.
