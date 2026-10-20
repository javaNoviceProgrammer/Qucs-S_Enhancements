# Coupled excitons: the TE line too weak (checked and fixed)

The report: the coupled-subband excitons predicted the multi-quantum-well modulator's TM absorption well, but its TE absorption much too weakly, much worse than the two-parameter model. The suggestion was to look at the Coulomb attraction and the normalization. Then: benchmark the coupled excitons against the plots of Lumerical's "GaAs-AlGaAs Electro-Absorption Modulator" article.

**The verdict:** the attraction and the normalization were right. Three other things made the TE line weak, and one of them broke the spectrum outright at high fields:

1. **A bug: at high fields the spectrum's subbands followed the wrong states along k.** Fixed.
2. **Missing physics: the electron and hole were not drawn together along the growth axis.** Added.
3. **Too few subbands for the attraction**, the Lumerical example's 2 valence subbands. Now said, with what to take.

With all three dealt with, the coupled excitons reproduce the article's **measured** hh-exciton transmission to 1–4 % at all three biases. Lumerical's own simulation is 18–27 % over the measurement, the two-parameter model 39–47 % over.

The well used throughout is `~/QucsWorkspace/lumerical_gaas_eam/qucs/eam_qw1.qfem`: one of the article's two 9.4 nm GaAs wells in Al0.3Ga0.7As, with Lumerical's material values, 4x4 bands, εr 10 for the excitons and a 6.5 meV Lorentzian. Everything ran over `qucs-s --mcp-server`, with an isolated home and cache, on copies of it.

## What was checked and found right

- **The normalization.** With the attraction switched off (εr 1e5), the coupled spectrum is the free spectrum to 0.05 % in TE and in TM, from the edge to 0.2 eV above it.
- **The attraction.** For one pair (the e1–hh1 of parabolic bands), the solver's own envelopes and reduced mass were fed to an independent real-space solve: the z-averaged potential and the 1s radial equation on a fine grid. It gives a 1s binding of 11.902 meV against the coupled solver's 11.906, and |ψ(0)|² 4.0417e15 against 4.04e15 m⁻². The test `onePairAgainstRealSpace` already checked an infinite well the same way.
- **The k·p channels and gauge.** Rotating each Kramers doublet so that one state carries all the e+↑ (bright TE) component changes the spectrum in the fourth digit. Without band mixing (γ2 = γ3 = 0) the k·p excitons are the parabolic ones.

## What was wrong

### 1. High fields: the subbands followed by index (a bug)

The k·p bands store each k's states by energy. A subband the spectrum takes (`select: localized`, `vstates`) was its index at k = 0. At 99 kV/cm the barrier's notch holds valence states above the well's hh1, and they cross it along k. Past the crossing, "hh1" was a notch state.

- **Free TE absorption** near the e1–hh1 edge kept only **0.15–0.31** of what every state gives.
- **The coupled exciton** lost its line: its TE peak was **0.07** of Lumerical's at −10.25 V.
- **The two-parameter model's** e1–hh1 globals (`qw.Eb`, `qw.Ex`) were NaN there: they were taken from the first two states, which here were the notch's.

**Now each direction's states are followed along k by their overlaps** (`KpBands::order`: largest |⟨a|b⟩|² first, among the 8 on either side by energy). The spectrum's pairs, the coupled excitons' spinors and the kept subbands' carriers all go through it. The E–k outputs (`qw.Ev1`…) stay by energy, as their help says. Both models' globals now come from the first kept pair.

### 2. The electron and hole not drawn together along the axis (physics added)

The coupled model writes the exciton on products of subband envelopes, χe(ze)·χh(zh). The attraction pulls the two toward the same z where they meet (its cusp). Products reach that only through many more subbands. In the parabolic case the 1s line is:

| Basis | Line |
|---|---|
| One pair | 0.70 |
| 12 pairs (the cap) | 0.86 |
| 48 pairs | 0.90 |

These are fractions of a real-space solve drawing the two together, converged with three functions. The two-parameter model has it built in (its γ), and it was validated against Khramtsov et al.'s exact solution.

**Now each of the lowest four pairs has a second function: itself times e^(−|ze − zh|/σ).**
- **σ** is the lowest pair's spread of ze − zh.
- **Overlap and z-energy:** for eigenstates f, f′ along the axis and one g, ⟨f g|H|f′ g⟩ = ½(ε + ε′)⟨f g|f′ g⟩ + ⟨f|c g′²|f′⟩, with c the ħ²/2m along the axis. This holds exactly for k·p spinors too, with c their kz² coefficients: e± γ1 − 2γ2, Z γ1 + 4γ2. With a product, which is an eigenstate, it is its energy times their overlap.
- **Attraction:** the products' form factors at q + β and q + 2β.
- **Optical amplitude:** the product's, since the two meet at one z either way.
- **Solve:** H u = E S u.

The new spectrum option `excitondrawn` is on by default; off gives the products alone. The log gives σ.

| Check | Products alone | Drawn together | Reference |
|---|---|---|---|
| Infinite 10 nm well, one pair: binding, |ψ(0)|² | 8.4470 meV, 3.2865e15 | 8.4922 meV, 4.0032e15 | real-space solve of the same two functions: 8.4904, 4.0030e15 |
| The modulator's well, one pair (parabolic): binding, |ψ(0)|² | 11.906 meV, 4.04e15 | 12.037 meV, 5.488e15 | real-space, the same: 12.034, 5.489e15; converged 12.106, 5.78e15 |
| Khramtsov et al.'s wells, 2–20 nm: binding against their exact solution | within 3.7 % | within 1.5 % | (two parameters: 1.2 %) |
| Their radiative decay ħΓ0 at 14 and 20 nm (measured 36.6, 35.4 µeV) | 23.0 µeV at 14 nm | 32.6, 30.7 µeV | (two parameters: 36.1, 33.9) |

One function gives about 95 % of the converged line: 0.95 in the modulator's well and 0.97 in the infinite one.

### 3. Too few subbands for the attraction (now said)

Band mixing moves hh1's TE strength into other subbands as k grows. If those are not in the coupled Hamiltonian, the 1s line has to draw its strength from the hh1 continuum near the edge, so the continuum dips below the free one.

This was checked against an independent real-space k·p exciton: the axial 4x4, with 16 hole states at k = 0 as its basis. It is the reviewer's reference from the a4ff84a check, extended here to the continuum. On a 10 nm GaAs/Al0.3Ga0.7As well, TE 20–50 meV above the e1–hh1 edge relative to free:

| Valence pairs coupled | TE / free |
|---|---|
| 2 (hh1, lh1) | 0.91–0.94 |
| 4 | 1.02–1.08 |
| 8 | 1.06–1.11 |
| independent reference | 1.14–1.10 |

In the modulator's well, hh2 lies only 8 meV below lh1, and the k·kz terms mix them. Taking it (`vstates` 3) makes the lh1 exciton's TM line 28 % stronger and TE 4 %. With 4 pairs, TM is +36 % and TE +10 %.

**The spectrum's selection is kept as given.** When the next valence subband in the wells lies within 30 meV below the last one taken, the coupled excitons now say so: "the coupled excitons take the spectrum's 2 valence subbands; the next in the wells, 8.06 meV below the last taken, mixes with them along k within the excitons' reach - their lines want it: "vstates" 3 or more…". The bands solve one pair more for this.

## The benchmark: Lumerical's electro-absorption modulator

Lumerical's curves were digitized from the article's figures (`~/QucsWorkspace/lumerical_gaas_eam/lum_digitized.npz`). The stack's absorption is taken as two times one well's, normalized to the stack's 78.8 nm. The transmission is −ln T = α Γ L, with Γ 0.0606 and L 150 µm (Lumerical's own, from its figures). Each figure is the peak of the hh exciton (TE) or the lh exciton (TM); in brackets, its ratio to Lumerical's, or for −ln T to the measured.

| bias | curve | Lumerical | coupled, before | two parameters (tracked) | coupled, fixed: 1 + 2 subbands | coupled, fixed: 1 + 4 subbands |
|---|---|---|---|---|---|---|
| 0 V (14.1 kV/cm) | TE hh exciton (1e5/m) | 4.81 at 1.4552 | 3.02 (0.63) | 5.90 (1.23) | 3.53 (0.73) | 3.87 (0.80) |
| | TM lh exciton (1e5/m) | 4.47 at 1.4684 | 3.78 (0.84) | 10.83 (2.42) | 4.47 (1.00) | 6.06 (1.36) |
| | TE −ln T (measured 3.65 at 1.4596) | 4.37 (1.20) | 2.75 (0.75) | 5.36 (1.47) | 3.21 (0.88) | **3.52 (0.96)** |
| −5.5 V (60.2 kV/cm) | TE hh exciton | 3.62 at 1.4488 | 2.21 (0.61) | 4.27 (1.18) | 2.69 (0.74) | 3.01 (0.83) |
| | TM lh exciton | 4.13 at 1.4652 | 3.38 (0.82) | 9.49 (2.30) | 4.10 (0.99) | 5.37 (1.30) |
| | TE −ln T (measured 2.78 at 1.4533) | 3.29 (1.18) | 2.01 (0.72) | 3.88 (1.39) | 2.44 (0.88) | **2.74 (0.98)** |
| −10.25 V (99.3 kV/cm) | TE hh exciton | 2.42 at 1.4390 | 0.16 (0.07) | 2.77 (1.14) | 1.68 (0.69) | 1.87 (0.77) |
| | TM lh exciton | 3.52 at 1.4586 | 1.22 (0.35) | 7.51 (2.13) | 3.34 (0.95) | 4.19 (1.19) |
| | TE −ln T (measured 1.72 at 1.4436) | 2.19 (1.27) | 0.14 (0.08) | 2.52 (1.46) | 1.52 (0.89) | **1.70 (0.99)** |

- The positions agree with Lumerical's to 1–3 meV. The measured peaks lie 4–5 meV higher than both simulations.
- **TE:** the coupled excitons with 4 valence subbands meet the measured hh exciton to 1–4 %. Lumerical's simulation, with the same 6.5 meV line, is 18–27 % over.
- **TM:** there is no measurement. The coupled excitons with Lumerical's own 2 subbands match its TM to 0–5 %. With the hh2 the lh1 exciton mixes with, they are 19–36 % above it. The two-parameter model's lh exciton (its k = 0 weights, its k·p mass) is 2.1–2.4 times Lumerical's.
- The peak heights rest on the 6.5 meV broadening Lumerical took; the measured line's own width is not known. The agreement is therefore of the strength at that width.
- Time per field: 19 s with 1 + 2 subbands, 28 s with 1 + 4.

## How it was checked

`test_fem_exciton`:
- `onePairAgainstRealSpace`: both ways, products alone and drawn together, against real-space solves of the same functions.
- `subbandsAgainstKhramtsov`: drawn together, one pair's line is within 3 % of 4 states' at 10 nm and 7 % at 20 nm.
- `kpBandsCoupled`: its binding is now 8.721 meV (the products alone, 8.687).
- `aHighFieldSubbandFollowed` (new): the modulator's well at 99 kV/cm. The localized 1 + 2 subbands' free TE is within 0.18 % of every state's, and the coupled binding within 2 % of the two-parameter one (11.23 against 11.45 meV).
- `tooFewSubbandsSaid` (new): 2 against 4 valence pairs, and the warning.

`test_fem_quantum`:
- `excitonsAgainstKhramtsov`: the coupled model added. Its binding is within 1.5 % of the exact; its radiative decay within 15 % of the measured (11 and 13 %).

Ten breaks, all caught:
- no pairs drawn together;
- no cost along the axis for drawing them together;
- the attraction not shifted (q for q + β);
- a drawn function dark;
- the overlaps ignored;
- σ from the electron alone;
- the subbands by index again;
- the globals of the first two states again;
- no next pair solved;
- the warning for 3 meV only.

## What it does not do

- **One function draws them together:** about 95 % of the converged line. Each pair has one β (1/σ), so that ⟨f g|H|f′ g⟩ stays exact. Two or more per pair would need the antisymmetric term between pairs, (β − β′) ∫ c (f1′f2 − f1f2′) g g′, and for k·p its k-linear part too.
- **The selection is the user's:** the coupled excitons do not take subbands that were not asked for. They say when the next one lies within 30 meV.
- At 99 kV/cm the spinors' basis holds 82 % of the worst state, at the bands' largest k. Twenty functions per J_z instead of twelve change the spectrum by under 0.1 %.
