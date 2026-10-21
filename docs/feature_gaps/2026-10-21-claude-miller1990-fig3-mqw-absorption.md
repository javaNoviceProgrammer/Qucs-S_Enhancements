# Miller 1990 Fig. 3: the check of 21 October

The check (`qucs-reviews/2026-10-21-claude-miller1990-fig3-mqw-absorption.md`) set the quantum wells against the room-temperature absorption of 65 periods of 96 Å GaAs / 98 Å Al0.3Ga0.7As (D. A. B. Miller, Optics & Photonics News, February 1990, Fig. 3). Its project is `~/QucsWorkspace/david_miller_paper/`.

**Its verdict:**
- n = 1 and n = 2 were reproduced within some 10 % after a uniform 10 meV shift.
- The library's AlGaAs gap was 43 meV high.
- n = 3 and everything above 1.6 eV were not reproduced.
- It found five bugs on the way and asked for six features.

**Now:** each bug is fixed, the gap has a measured fit, and each wish is met, as below.

## The bugs

| Review | Now |
|---|---|
| **2.1** Under `valence: kp8` the 8x8 found an electron at 0.430 eV, below the GaAs conduction edge (0.624), and others that matched no level. The spectrum then had "0 transitions" (α zero everywhere), said only in the log | **The cause:** a band of spurious states. Ep lowered for A_c 1 alone (21.19 eV for GaAs) left the light holes' remote term along the axis, γ1 + 4γ2 − Ep/Eg, at −0.48 with Chao and Chuang's γ1 6.8 and γ2 1.9. So the Z orbital's band turned up into the gap at large kz (some 33/nm). Its states were 70 % valence, wherever their energy fell: 0.203 and 0.655 eV here. Among the electrons they shifted every Kramers pair by one, and the confinement checks then dropped every transition. **Now:** <br>• **Ep is lowered for the light holes too**, until that term is at least a free electron's, as A_c already is; GaAs here gets 19.06 eV. The log names it: "domain 2's Ep 28.8 eV to 19.06 eV (its light holes', γ1 + 4γ2 14.4)". Miller's well has no spurious state left; its electrons are 0.6557 and 0.7420 eV against the effective mass's 0.6551 and 0.7451 (e2 lower: not a parabola); its spectrum has 56 transitions, in 67 s (98 s and none before). <br>• **What Ep cannot help** is left out: a material's own parameters can still leave spurious states, such as an in-plane non-ellipticity (γ3 large). A state counts as its band's only if it is mostly of that band, on its band's side of the edges, and its own part smooth along the axis (ħ²kz²/2m0 under 2 eV). The search then looks again with more states, or, if still short, takes the nearest of those left out, so each k keeps its states; the log says how many. The conduction states' shift follows them as they rise with k, so a spurious band near the gap no longer fills the search (none of the electrons were found at 2.66/nm before). <br>• **A spectrum of no transitions is a warning**: "a spectrum of no transitions between the subbands - its absorption the barriers' bulk absorption above their gap alone: see which subbands were left out ..." |
| **2.2** The States step gave the same electron levels under kp8 as under kp4 | Under the 6x6 and 8x8 the States step takes the k·p's energies at k = 0: the 8x8's electrons, and the heavy and light holes sorted by their Z orbital's share (a heavy hole's none). Their envelopes stay the effective mass's, and the log says so ("the 8x8 k·p's energies at k = 0 for 4 electrons, 4 hh, 4 lh"). The 4x4's are the effective mass's at k = 0 anyway, and stay so. Miller's well: e1 0.65559 and e2 0.74682 eV, against 0.65523 and 0.75099 under kp4 |
| **2.3** One sweep point (x 0.24, barriers 30 nm) reported an exciton bound by 49.6 eV, its spectrum normal | **The cause:** the coupled excitons' hole spinors are taken on a basis of 48 functions, and that basis held some of the 14 valence subbands' states little at the grid's top (0.53 %). Their projections there were noise, 0.75 from orthogonal, though the excitons take their products as orthonormal. The overlaps of the pairs drawn together then lost their positiveness (two eigenvalues −0.037), and the generalized solve broke, giving a line 62.6 eV down. **Now:** <br>• the spinors are made orthonormal at each k (Gram and Schmidt's, those the basis holds most of first, so those it holds stay as they were). The point is bound by 8.725 meV, as the sweep's others (8.70-8.76). <br>• Should a k point's overlaps still not be positive, its functions drawn together are left out there, its products kept: the excitons there are as without them, and the log says so |
| **2.4** `vstates` 120, `cstates` 60 gave 50 each, without a word | A count past its most is taken as the most and said as a warning ("its vstates 120 taken as 50, the most it takes"). This covers a spectrum's states, cstates and vstates (50), a States step's states (100), a Valence Bands step's states and points (50, 2000), and the coupled pairs (64) |
| **2.5** The coupled excitons' warning kept asking for one more `vstates`, chasing box states in the barriers | The warning's next state must be in the wells: mostly in them, and under "bound" above the barriers' valence top. Its distance is measured from the last state taken in them, and its count includes the states passing up to it. Miller's well with 14 valence subbands (6 in the well) now says nothing |

## The data

| Review | Now |
|---|---|
| **3.1** The library's Al0.3Ga0.7As gap is 1.841 eV at 300 K; the measured one is 1.798 (Aspnes, Casey and Panish: 1.424 + 1.247x) | The 300 K fit (`bands: measured`) takes Casey and Panish's: 1.424 + 1.247x below x 0.45, and 1.147 (x − 0.45)² more above it (1.7981 eV at x 0.3, 2.1980 at 0.6). Its temperature dependence is Vurgaftman's. The default stays Vurgaftman's, and its help says the 1.841. With it ΔEc is 57 % of ΔEg, Miller's own value. The ternary inside AlGaInAs is unchanged, since that quaternary's fit rests on it (0.954 eV kept) |

## The wishlist

| Wish | Now |
|---|---|
| **4.1** A periodic (Bloch) boundary: one period standing for the superlattice | **The line's ends** (Quantum Wells, 1D): `walls` as before, or `periodic`. The two ends become one point, and the states are the superlattice's at the centre of its mini-zone (kz 0). One period drawn stands for the whole stack; M periods drawn give M points of the mini-zone (kz 2πj/ML), with no transitions between them (their overlaps cancel over the cell), so the steps take their minibands' widths. A field along the axis is refused, since the potential must repeat. There are no walls for the spectrum's wall cutoffs to measure. Checked against Kronig and Penney's equation (below) |
| **4.2** Coupled excitons on superlattice and above-barrier states | On a periodic line the coupled excitons take the period's states, a superlattice's. A period of a 10 nm well in 30 nm of barrier binds its e1-hh1 at 8.5303 meV, as the single walled well. **How many pairs are coupled** is given now (`excitonpairs`): the default stays 12 of each kind of hole (k·p: 16), up to 64, so a superlattice's minibands above the barriers (n = 3) can be among them |
| **4.3** The barriers' own absorption edge at 300 K, with its exciton and Urbach tail | Under `continuum: bound`: <br>• **The bulk's excitons** (`bulkexcitons`, Elliott's): below the gap the 1s, 2s, ... lines, each material's Rydberg (its reduced mass in its permittivity, heavy and light holes apart); above it Sommerfeld's factor 2π√(R/ε)/(1 − e^(−2π√(R/ε))) on the √ε. <br>• **Urbach's width** (`urbach`): the 3D lines broadened as e^(−\|x\|/E_U)/2E_U, the exponential tail below the gap; a GaAs slab's 52 and 42 meV under its gap are e^(10/6) apart, as the tail should be. Both are off by default |
| **4.4** The 8x8 with excitons | The 8x8's pairs have their excitons: each pair's own 1s (two parameters, or Bastard's), its electron the 8x8 state at k = 0 (all eight components) with its in-plane mass. A well's e1-hh1 is bound by 9.33 meV under the 8x8 and 8.89 under the 6x6, which matches the reduced masses' 1.10. The coupled model asked for under the 8x8 takes the per-pair one, said: its spinors are the 4x4's and 6x6's alone (they were left out, "not modelled") |
| **4.5** Broadening growing with energy, or a width per transition | `linewidthslope`: the lines' width at a photon's energy is the broadening plus this times ħω above the lowest edge (0.05: 5 meV wider 0.1 eV above). A line near ħω has the width there, each line its own; the log says it. A spectrum with it equals, at each energy, the spectrum of that fixed width, to 1e-9 |
| **4.6** A validated absolute strength (W7 of the 20 October review: TE peaks 0.72-0.77× Lumerical's) | **Checked against a measurement, through the bulk:** <br>• **The bulk against Aspnes and Studna's GaAs** (Phys. Rev. B 27, 985, 1983; the library's fit α = 6.578e4/cm √(E − Eg)/E: 15.1, 18.3 and 20.3 ×10³/cm at 1.55, 1.63 and 1.70 eV). Elliott's 3D absorption (Kane's Ep 28.8 eV, n, \|Mb\|² as the wells take them; Urbach 6 meV) gives 1.094, 0.989 and 0.939 of it. The free carriers' √ε alone gives 0.666, 0.664 and 0.664. <br>• **The wells against the bulk:** a wide well's free absorption per its thickness converges on the bulk's free absorption, 0.94, 0.97 and 0.98 of it at 30, 40 and 60 nm (1.63 eV): their 2D steps sum to its 3D density of states. <br>So the wells' absolute strength holds against a measured absorption, within 10 %, once the excitons' Sommerfeld factor is in. Lumerical's TE some 1.3 times higher was not run here: not settled |

## Miller's Fig. 3 with them

One period, its ends periodic: 4.9 nm of Al0.3Ga0.7As, 9.6 nm of GaAs, 4.9 nm more. The setup:
- `bands: measured` (barrier gap 1.798 eV), Chao and Chuang's Luttinger parameters, 4x4;
- coupled excitons on all states (`continuum: states`), 4 conduction and 10 valence subbands, 24 pairs coupled;
- a 5.5 meV Gaussian line with `linewidthslope` 0.05, α per 19.4 nm period.

It took 2 minutes in the app.

| | QFEM | Fig. 3 |
|---|---|---|
| n = 1 hh | 1.455 eV, 11.3 ×10³/cm | 1.466, 11.6 |
| valley | 1.525, 5.5 | 1.53, 6.0 |
| n = 2 | 1.570, 9.4 | 1.582, 12.6 |
| plateau after it | 1.610, 8.0 | 1.63, 12.2 |
| n = 3 | a line at 1.745, 8.7 | 1.710, 18.2 |

- **The 10 meV offset:** the same uniform 10 meV low at n = 1 and n = 2 as the check found.
- **The fit:** the check's own, a α + b + c (E − 1.45) over 1.44-1.66 eV with QFEM shifted 10 meV, gives a = 0.99 and c 21 per eV, rms 0.62 ×10³/cm (the check's 0.65).
- **n = 3:** there now, as a line at the superlattice's Γ point, but 35 meV above the measured. The check's 7-period box had its step at 1.72-1.73; one period is one point of each miniband, and M periods drawn sample them.
- **The barriers' edge:** under `continuum: bound` with the bulk's excitons and Urbach 10 meV, it rises from 1.76 eV (7.0 to 8.8 at 1.78). The measured rises from 1.74 to over 24: the excess above 1.6 eV the check found above the bulk-GaAs ceiling remains outside a model of the wells.

## Tests

| Test | What it holds |
|---|---|
| `test_fem_kp8 aGaAsWellOfChaoAndChuang` | Miller's well under kp8: Ep lowered for the light holes, said; no spurious state; the electrons above the well's edge, e1 within 2 meV of the effective mass's, e2 below it; transitions; the States step's 8x8 energies equal to the spectrum's, the 4x4's the effective mass's |
| `spuriousStatesLeftOut` | A γ3 of 10: spurious states left out and said at 20 of 21 values of k, none taken as electrons; transitions; the electrons above the conduction edge, the holes below the valence's |
| `noTransitionsSaid` | No electron kept (a wall cutoff of 0): the warning |
| `kp8EdgeStep` | The 8x8's e1-hh1 exciton bound 1.0-1.1 times the 6x6's; the coupled model asked, the per-pair one said |
| `test_fem_exciton aSweepPointsExciton` | The sweep's point: bound by 8.74 meV, no k point falling back |
| `overlapsShortOfPositiveLeftOut` | Two pairs of one function, drawn together: every k point falls back, the states those of the products alone to 1e-9 |
| `boxStatesNotAskedFor` | 14 valence subbands, 6 in the well: no warning |
| `aSuperlatticesExcitons` | A periodic period's e1-hh1 the walled well's to 1 % (equal) |
| `pairsCoupledGiven` | 2 pairs of each kind coupled, said; 80 taken as 64, said |
| `theBulksAbsoluteStrength` | The bulk against Aspnes and Studna's GaAs (Elliott's within 10 %, the free carriers' 0.6-0.72); wide wells to the bulk |
| `theBulksEdge` | Urbach's tail e^(E/E_U) to 1 %; without it, nothing below the gap |
| `test_fem_quantum countsPastTheirMostSaid` | The caps said |
| `theAlloysAt300K` | AlGaAs's measured gap at x 0.3 and 0.6 to 1e-9, its temperature dependence Vurgaftman's, the default Vurgaftman's 1.841 |
| `periodicEnds` | Kronig and Penney's roots at cos kzL = 1 (one period) and −1 (two) to 20 µeV; a field refused; a wall cutoff leaves nothing out |
| `linesWiderWithEnergy` | The grown width the fixed one's at each energy to 1e-9 |

**Under the sanitizers** (Debug, as CI's Linux job), each function was timed:
- `periodicEnds` 25 s, `linesWiderWithEnergy` 8 s, `countsPastTheirMostSaid` 11 s, `theAlloysAt300K` under 1 s;
- `overlapsShortOfPositiveLeftOut` 11 s, `pairsCoupledGiven` 147 s, `theBulksAbsoluteStrength` 36 s, `theBulksEdge` 16 s;
- `noTransitionsSaid` 9 s; `kp8EdgeStep` on a lighter path (the 8x8's spectrum alone, its excitons bound, k at 21 values).

Some are skipped there, as an existing 8x8 test is, and the Release jobs run them:
- the 8x8 spectra of `aGaAsWellOfChaoAndChuang` and `spuriousStatesLeftOut`, past QtTest's 300 s there;
- the 14-subband coupled excitons of `aSweepPointsExciton`, `boxStatesNotAskedFor` and `aSuperlatticesExcitons`.

The whole suite passes in Release (135 of 135).

## Break checks

Each change undone on its own, its tests built and run (Release). All 22 are caught:

| Break | Caught by |
|---|---|
| The light holes' Ep rule | `aGaAsWellOfChaoAndChuang` |
| No spurious state left out | `spuriousStatesLeftOut` |
| The conduction states' shift at the band's edge | `spuriousStatesLeftOut`: electrons taken all the same at 5 values of k |
| One search round, no top-up | `spuriousStatesLeftOut`: a crash (signal 11), its arrays short |
| No transitions not said | `noTransitionsSaid` |
| The States step's k·p levels not taken | `aGaAsWellOfChaoAndChuang`: 0.65523 against 0.65559 |
| The spectrum's own solve given them | `aGaAsWellOfChaoAndChuang` |
| The spinors not orthonormal | `aSweepPointsExciton`: k points fell back |
| No fallback where the overlaps are not positive | `overlapsShortOfPositiveLeftOut` |
| The vstates warning's next not in the wells | `boxStatesNotAskedFor` |
| A spectrum's counts capped silently | `countsPastTheirMostSaid` |
| A States step's count capped silently | `countsPastTheirMostSaid` |
| AlGaAs's gap Vurgaftman's in the fit | `theAlloysAt300K` |
| Periodic ends not one point | `periodicEnds`: 0.0833 against 0.0502 eV |
| A periodic line's field not refused | `periodicEnds` |
| A periodic line's walls measured | `periodicEnds`: its states left out |
| The coupled pairs 12 whatever is given | `pairsCoupledGiven` |
| excitonpairs past 64 not said | `pairsCoupledGiven` |
| The bulk's excitons not Elliott's | `theBulksAbsoluteStrength`: 0.666 |
| No Urbach line | `theBulksEdge` |
| The lines one width at every energy | `linesWiderWithEnergy` |
| The 8x8's pairs without excitons | `kp8EdgeStep` |

**Not caught at first, four:**
- **The spinors' orthonormality:** its test looked for the fallback's earlier wording.
- **The vstates warning:** its break left half of the old code.
- **The conduction shift and the top-up:** no case of the tests needed them once the light holes' rule removed the spurious band. The γ3 case now asserts no electron is taken all the same, and the top-up's break takes the search's extra rounds with it.

**Found on the way:**
- **A crash:** with a γ3 of 10 the old searches crashed, their arrays short at some k.
- **The valence side's spurious band:** it fills the valence search there too, 8 states at 2 values of k taken all the same. A shift following the valence states did not help, since that band lies among them, so it was left out.
- **A slowdown:** a first version sought four pairs more in every 8x8 search, in case spurious ones were among them. That made every 8x8 search 2.4 times as slow (an existing test, `kp8AgainstTightBinding`, past QtTest's 300 s under the sanitizers). Now more are sought only while too few pass.

## Not done

- **A Bloch phase within one period** (kz ≠ 0 on one cell): its states would be complex. M periods drawn sample the mini-zone instead, in real arithmetic.
- **The coupled excitons on the 8x8's states:** its electrons are spinors too, which the coupled model does not take. The per-pair model is taken, and said.
- **Urbach's tail on the wells' own lines:** it is the bulk's alone. The wells' lines keep the spectrum's shape, with `linewidthslope` for their widths.
- **The 1984 measurement** (Chemla et al., IEEE JQE 20, 265) that would settle Fig. 3's normalization and background: not at hand.
