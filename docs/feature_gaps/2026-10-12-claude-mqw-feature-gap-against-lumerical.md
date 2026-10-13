# Feature gaps: the MQW physics against Lumerical's MQW solver

*13 October 2026 - on `72dfff0`.*

The review compared the `qw` physics with Ansys Lumerical's MQW solver, from Lumerical's
documentation; Lumerical was not run. In its words, `qw` was "a generation behind" in
physics. Three things were missing: k·p valence-band mixing, excitons, and a
self-consistent Schrödinger–Poisson loop. Spontaneous emission, line shapes other than
the Lorentzian, a built-in Kramers–Kronig Δn, temperature checks and wurtzite were
missing too.

The user asked for all of it except wurtzite, which gets a written proposal:
[`docs/proposals/2026-10-13-wurtzite-quantum-wells.md`](../proposals/2026-10-13-wurtzite-quantum-wells.md).

Some of the review was already done when this round began:

- **The semiconductor round:** the Schrödinger–Poisson loop (C4).
- **The MQW calibration round** (`2026-10-12-claude-mqw-calibration.md`): the alloy gaps
  (D1), one gap in every physics (D2), the masses' weighting documented (D3), excitons
  and line shapes (C3.1, C3.2).

This round added:

- **k·p valence bands:** the 4x4 and the 6x6, solved on an in-plane k grid, with gain,
  absorption and emission integrated over k.
- **Their in-plane approximations and operator order:** the axial approximation
  (Lumerical's) and three other ways of taking the in-plane k; Burt and Foreman's operator
  order across interfaces.
- **A two-parameter exciton:** the one Lumerical's variational method uses.
- **Field-dependent broadening, the bulk continuum and the index:** broadening that grows
  with the field, the barriers' bulk absorption in place of the box's states, and the
  Kramers–Kronig index.
- **Spontaneous emission and temperature:** the emission rate, the emission coefficients
  and temperature checks.
- **Benchmarks against published results.**

Lumerical's own physics page, read for this round, decided several details below.

Along the way, faults were found and fixed (details under "Found along the way"):

- **The exciton line:** last round's 1s line was half as strong as it should be.
- **Spurious bands:** the symmetrized operator order put a spurious band into InGaAs/InP's
  gap.
- **The bulk continuum:** its gap moved with the field, and its rule for "bound" dropped a
  tilted well's own first states.
- **Degenerate k·p states:** one vector's Krylov space could miss some.

## The review's table (B) against now

| Feature | Lumerical MQW | Review: `qw` | Now |
|---|---|---|---|
| Valence bands | 4x4, 6x6 or 8x8 k·p | Uncoupled hh and lh, parabolic in the plane | **4x4 and 6x6 k·p** (`"valence": "kp4"`, `"kp6"`), solved at each in-plane k; the 8x8 is not built |
| Crystal types | Zinc blende and wurtzite | Zinc blende | Zinc blende; wurtzite **proposed** |
| Strain | Yes | Yes | Yes, and in the k·p bands too: the Z orbital's 3Qε, so lh − hh = 2Q in the 4x4, with the split-off band coupled in the 6x6 |
| Field / QCSE | Yes | Yes | Yes |
| Excitons | Direct or variational (two parameters) | No | **Variational, two parameters**, the default (Bastard's one-parameter form selectable); 2D Sommerfeld continuum; parabolic and k·p |
| Self-consistent Schrödinger–Poisson | Through CHARGE | No | Yes, since the semiconductor round |
| Gain | Yes | Yes | Yes, parabolic or k·p |
| Spontaneous emission | Yes | No | **`qw.rsp`, `qw.Rsp`, `qw.gspTE`, `qw.gspTM`** |
| Broadening | Several | Lorentzian only | Lorentzian, Gaussian, sech (last round); now **growing with the field** too |
| Δn | Yes, for modulators | Done by hand | **`qw.dnTE`, `qw.dnTM`** (Kramers–Kronig over the window, warned when the window's top still absorbs) |
| Temperature | Built in | Through material data, tested at 300 K only | Tested at 77 K too (gaps, states, Fermi statistics, gain, emission) |
| Wires and dots | 1D MQW | 1D wells, 2D wires, dot states | Unchanged; the k·p bands are 1D wells' |

## The review's proposals against now

| Review | Now |
|---|---|
| **C2.** Add a 4x4 Luttinger–Kohn Hamiltonian, or the 6x6 with the split-off band, solved on an in-plane k grid. Add γ3 and Δso to the materials. Integrate gain and absorption over k in place of the 2D parabolic density of states. | **Done.** See "The k·p valence bands" below. |
| **C3.1.** A 1s exciton per electron-hole pair: variational binding energy and oscillator strength, and the Sommerfeld-enhanced continuum. | **Done last round, improved now:** a second variational parameter, and the line twice as strong as last round's. See "Excitons". |
| **C3.2.** Gaussian and sech line shapes, the broadening possibly growing with the field. | Line shapes last round. **Now** `dlinewidth`: the half width grows by that much per V/m of the field across the wells (`qw.Fw`, the slope of their conduction band edge: the uniform field's, or another physics' potential's there), as Lumerical's EAM example fits Γ against the field. |
| **C3.3.** A built-in Kramers–Kronig Δn, TE and TM against field, over a stated window, warned when the window is too narrow. | **Done:** `qw.dnTE`, `qw.dnTM`. The window is the step's energies. A warning names the share of the largest absorption still at the window's top. See "The index". |
| **C3.4.** Absorbing or open boundaries for continuum states, or only bound transitions plus an analytic continuum. | **The second:** `"continuum": "bound"` keeps only transitions between states confined by their own well's barriers. Above the barriers' gap it adds their 3D absorption, over the line's length, per the wells' thickness. See "The bulk continuum" for what it does not do. |
| **C4.** A coupled loop: `semi` potential → `qw` states → quantum densities → `semi` Poisson. | Done in the semiconductor round: Schrödinger–Poisson regions and Self-consistent. |
| **D1-D3.** | Done in the MQW calibration round. |
| **F.** Rebuild one or two published Lumerical MQW examples and compare. | Lumerical's examples keep their stacks and results in downloads, and nothing was downloaded. Its GaAs/AlGaAs EAM example shows only plots. Instead, the published results below, and Lumerical's physics page for its models. |
| **Priority 5.** Wurtzite, spontaneous emission, temperature. | Emission and temperature done; wurtzite proposed. |

## The k·p valence bands

`"valence": "kp4"` or `"kp6"` on a quantum wells physics (1D wells) solves the holes as
k·p bands:

- **The Hamiltonian:** written in the X, Y, Z orbitals times spin, with Dresselhaus, Kip
  and Kittel's L = −(γ1 + 4γ2), M = −(γ1 − 2γ2), N = −6γ3.
- **Strain:** the Z orbital's 3Qε.
- **Spin-orbit:** Δ(σ·L − 1)/3.
- **The 4x4:** the projection on j = 3/2.

At each in-plane k it is solved by finite elements on the wells' line, by shift and
invert. The k grid runs from 0 past the highest photon by 20 broadenings and 10 kT, and far
enough to count the holes.

The eigenvalue search is Krylov and Schur's. One vector's Krylov space holds only one
direction of each degenerate set. A Kramers pair's other state, or the other three of
four (an isotropic band's), arrive only as rounding brings them. At one of the averaged
directions they did not, and that direction's spectrum was 3e-5 off. So after the first
search, a search of two states (to 1e-5) on what is M-orthogonal to all found checks
whether anything as high as the lowest wanted state is left. If so, a full search there
follows, then another check. Rayleigh and Ritz over all found keep the highest.

**Operator order.** Across interfaces, the X–Z coupling is written in Burt and Foreman's
order: kx N₊ kz + kz N₋ kx, with N₋ = M − ħ²/2m0 and N₊ = N − N₋.

- **What happened symmetrized:** with (N kz + kz N)/2, a 6 nm In0.53Ga0.47As well in InP
  grew a band from −0.61 eV at k = 0 to −0.095 eV at 2.08 nm⁻¹, into the gap. Its "holes"
  set the hole Fermi level 0.7 eV too high, and a 3e18 cm⁻³ well showed no gain at all.
- **Now:** in Burt and Foreman's order every band falls with k.
- **The check:** an InGaAs-like well in InP-like barriers against linear elements written
  in the test (Richardson's to 1e-8 eV) agrees to 8e-7 eV. Symmetrized, the two are 67 meV
  apart, and two states sit in the gap at 1 nm⁻¹.

**The in-plane k** (`"inplane"`):

- **Axial** (the default): the plane isotropic, R's γ2 and γ3 their mean γ̄. This is
  Chuang's and Lumerical's choice.
- **Along [100] or along [110]**, taken for every direction.
- **Averaged:** the warped bands averaged over the plane's directions, at four Gauss
  points from 0 to 45°. It costs four times the work.

At 0.6 nm⁻¹ the axial top band is within 0.5 meV of the true average, while [100] and
[110] are 14 meV apart: axial is a sound default.

**Spectra.** Each electron state is paired with each valence state at each k:

- **TE** = (3/2) Σ |⟨e|X, Y⟩|²;
- **TM** = 3 Σ |⟨e|Z⟩|²;
- **the measure** is (k² − k′²)/4π per interval;
- **the line:** each interval's energies spread under the line's shape. Unbroadened, it
  is taken where its energy is the photon's, with the electron's and hole's energies
  interpolated in k², so that emission and absorption keep their exact relation.
- **the tail:** past the grid's top, the line's tail, as the parabolic transitions have it;
- **the holes' Fermi level:** from the bands themselves. Each interval's 1 − f is
  integrated with its energy linear in k², kT [ln(1 + e^((E − F)/kT))]: exact for a
  parabolic band, where the trapezoids had been 31 µeV off.
- **near the photon:** each interval in cells of at most a quarter of kT or of the line's
  width, each cell with its own occupation.

A **Valence Bands** step gives the dispersion: `qw.k`, `qw.Ev1`, ... with Kramers' pairs
both.

What it changes. These are InGaAs wells, 6 nm, on InP, at 3e18 cm⁻³ and a 6 meV broadening:

| Well | Parabolic | k·p 4x4 | k·p 6x6 |
|---|---|---|---|
| In0.68Ga0.32As (≈1 % compressed): TE gain's peak | 5124 /cm | 2987 /cm | 2578 /cm |
| In0.38Ga0.62As (≈1 % stretched): TM gain's peak | 6570 /cm | 665 /cm | 241 /cm |

Mixing makes the hole states denser in the plane, so the parabolic holes overstate the
gain, as the review expected (C2).

The stretched well's top band is light-hole-like. It rises 3.5 meV from k = 0 to 0.4 nm⁻¹
before it falls: the camel's back of tensile wells. Its density of states holds the holes
back.

TE still dominates the compressed well and TM the stretched one in every model.

**Cost.** A first search takes 120–280 shift-inverted products per k, and the check adds
some. The step's `kpoints` (121 by default) sets how many values of k. A 6x6 spectrum of a 26 nm stack with the line's 0.1 nm elements takes tens of
seconds. With `"h1d": "0.25[nm]"` it takes a third of that, and the gain's peak changes by
1 /cm in 2980. The agent reference says so.

A block Krylov method from the last k's states was tried first. It holds degenerate sets
whole by construction, but it took 8–14 turns per k (about 640 products) in the strained
wells: their subbands crowd the top.

## Excitons

**Two parameters.** The trial function is
χe(ze) χh(zh) e^(−√(ρ² + γ²(ze − zh)²)/λ): Gerlach's form, Khramtsov et al.'s, and the
one Lumerical's variational method uses. The electron and hole are drawn together along
the axis as well as in the plane.

Its energy has closed forms in the exponential integral E1. The exception is the Coulomb
term, tabulated in (s/λ, γ) and interpolated bicubically: bilinear interpolation left a
minimum at every node of the table.

It is minimized over λ and over γ from 0 to 1.5. At γ = 0 it is Bastard's form.

The line's strength is |F(0)|² = 2/(πλ² Σ P n), where n is the in-plane normalization at
each separation.

- **Selecting it:** `"excitonmodel": "bastard"` keeps the one-parameter form.
- **Outputs:** `qw.gX` gives γ.
- **The k·p path:** the hole's mass along the axis comes from each state's Z share
  (0 for a heavy hole, 2/3 for a light one).

**The line's strength, corrected.** Last round's 1s line was 2/(πλ²) strong. That counts
one spin pair, but the continuum beneath it, mr/(πħ²), counts two. It is twice that now,
and two published checks agree:

- **Elliott's 2D limit:** the 1s line carries 8 Eb of the bare step. Measured from the
  spectrum, it is 0.959 of that in a 0.5 nm well, 0.927 at 1 nm and 0.826 at 4 nm,
  approaching 1 as the well thins. Half the strength would give 0.48.
- **Khramtsov et al.'s measured radiative decay** (below).

The earlier write-up has a correction note.

**Benchmark: Khramtsov et al.** "Excitons in square quantum wells: microscopic modeling
and experiment" (arXiv:1508.00480) solve the heavy-hole
exciton exactly, as the 3D Schrödinger equation. They also measure its radiative decay
by reflectance. Their parameters for GaAs/Al0.3Ga0.7As (Table I) were used as they are,
and their exact binding is given by the paper's fit Rx(L), good to 0.07 meV.

| Well | Exact | Two parameters | Bastard's | ħΓ0 measured | two parameters | Bastard's |
|---|---|---|---|---|---|---|
| 2 nm | 9.493 meV | 9.420 | 9.261 | | | |
| 5 nm | 9.457 | 9.344 | 9.218 | | | |
| 10 nm | 8.362 | 8.261 | 8.085 | | | |
| 14 nm | 7.666 | 7.591 | 7.350 | 36.6 ± 0.6 µeV | 36.1 | 28.8 |
| 20 nm | 6.911 | 6.831 | 6.513 | 35.4 ± 0.4 µeV | 33.9 | 24.2 |

- **Binding:** two parameters are within 1.2 % of the exact; Bastard's within 5.8 %.
- **Radiative decay:** ħΓ0 = Lw ∫α₁ₛ dE/2π here. Ivchenko's absorbance of one well,
  2Γ0Γ/((ω − ω0)² + (Γ + Γ0)²), integrates to 2πΓ0, and with the paper's |p_cv|² = m0Ep/2
  this equals their formula with our envelope. Two parameters are within 4.2 % of what
  was measured; Bastard's are 32 % low.

**Excitons in the k·p bands.** With γ2 = γ3 = 0 the k·p spectrum with excitons is the
parabolic one's to 1.5e-7 of its peak, and the bindings agree to 1e-6.

## The index (Kramers–Kronig)

`qw.dnTE` and `qw.dnTM` are (cħ/π) P∫ α(E′)/(E′² − E²) dE′ over the step's energies. The
principal value is taken by subtracting α(E), whose own integral is analytic.

- **Between two fields** only the change counts: electrorefraction's Δn.
- **The warning:** when the window's top still absorbs more than 5 % of its most, the
  log says so: what changes above the window is left out.
- **The check:** on the infinite well's Lorentzian steps, against the same principal value
  of their analytic α on 40,001 points, it agrees to 1e-3 of the largest index.

Lumerical computes the index differently: from each transition's complex susceptibility,
over its k grid (its page asks for about 30 % of the Brillouin zone for it to converge).

## The bulk continuum

`"continuum": "bound"` keeps the transitions of states confined by their own well. A
state counts as confined when it lies below (a hole's: above) the barriers' band edge
just outside the stretch of well its mean position is in, on whichever side confines
less.

At first it was judged by the band edge at the line's far ends, as `qw.bound_e` is. A
field lowers those ends: in a 10 nm GaAs well with 15 nm barriers at 100 kV/cm they drop
by 0.2 eV. That dropped e1-hh1 itself, and with it the exciton, in exactly the modulator
case the option is for. A state above the far ends but below its well's own barriers is
quasi-bound: it tunnels out only through the triangular barrier. `qw.bound_e` keeps its
meaning.

Above the barriers' gap it adds their 3D absorption,
(π q² ħ/(n c ε0 m0² E)) |Mb|² Σ (1/2π²)(2mr/ħ²)^(3/2) √(E − Eg), broadened, over the
line's length and per the wells' thickness.

- **The check:** a barrier alone (no well, so nothing confined) absorbs exactly that, to
  1e-3, at 0 and at 100 kV/cm. The gap is taken at the line's end (its own Ec − Ev), not from the band
  edges' extremes. Those moved by the field's drop, 0.28 eV here, and an early version
  had that bug (below).
- **The well:** a GaAs well in Al0.3GaAs absorbs, 559 meV above the barriers' gap, 1.21
  times its barriers' bulk TM; the rest is the bound light holes' steps.
- **In a field:** at 100 kV/cm the 10 nm well keeps its e1-hh1 transition and exciton
  (32 transitions in all).
- **No sum rule:** a state that a field frees takes its steps with it, to every energy,
  and a bound transition's overlap that falls takes some of its step away, to every
  energy. Δn below the edge then still depends somewhat on the window's top. This is
  documented, not hidden.

## Spontaneous emission

The rate is r_sp = (n² E²/(π² ħ³ c²)) (2 α_sp,TE + α_sp,TM)/3 per volume, time and
photon energy (`qw.rsp`, 1/(s cm³ eV)), and over the energies `qw.Rsp`. Here α_sp is the
gain's own sum with f_c (1 − f_v) for f_v − f_c. `qw.gspTE` and `qw.gspTM` are those
coefficients themselves, Lumerical's g_sp.

**The check:** unbroadened, emission is absorption over e^((E − ΔF)/kT) − 1, for each
polarization, parabolic and k·p, at 300 K and 77 K, to 1e-12.

## Temperature

The physics' `T` moves the compounds' gaps (Varshni) and the carriers' statistics. At 77 K:

- **The gap:** GaAs's is 1.50760 eV, Varshni's, against 1.508 measured.
- **The states:** a 10 nm GaAs/Al0.3GaAs well's e1 and hh1 are BenDaniel and Duke's of the
  77 K bands, to 1e-7.
- **Carriers and gain:** the electrons' sheet is exact; the gain crosses 0 at ΔF; emission
  keeps its relation.
- **77 K against 300 K:** the gain's peak at 3e18 cm⁻³ is 5357 /cm against 2769 /cm.

## What Lumerical does that is still different

The comparison is from Lumerical's "MQW Solver Physics" page, read in a browser.

- **The 8x8 k·p:** the conduction band coupled in. Not built; at near-infrared gaps the
  6x6 and a parabolic conduction band are usual.
- **Excitons by its direct method:** in momentum space, with heavy-hole–light-hole
  mixing. Here they are variational. Lumerical's example says its variational method
  matches the direct one's peak energies to 1 % but overestimates the absorption's
  magnitude.
- **Its Sommerfeld factor:** it uses the 3D Rydberg. Here R = Eb/4, the bound exciton's
  own (the same in the strict 2D limit).
- **Its broadening Γ:** a full width. Here `linewidth` is the half width.
- **The complex index:** from the susceptibility, not from Kramers and Kronig over a
  window.
- **Wurtzite:** proposed.

## Examples

Two examples were added to `examples/multiphysics/`. `theExamples` loads and checks both.

- **`kp_laser_gain.qfem`:** the 980 nm pump laser's In0.2Ga0.8As well with the 6x6
  bands.
  - The Valence Bands step draws the dispersion; the top band falls with k.
  - Gain at 3 and 5e18 cm⁻³: 1280 and 2482 /cm at 982 nm, with the spontaneous emission.
  - At 2e18 cm⁻³ the parabolic holes of `strained_laser_gain.qfem` give gain; the k·p
    holes still absorb.
  - About 10 s.
- **`gaas_eam_excitons.qfem`:** a 10 nm GaAs well in Al0.3GaAs at 0, 50 and 100 kV/cm, with
  two-parameter excitons, a Gaussian line growing by 3 meV per 100 kV/cm, the bulk
  continuum and the index.
  - The exciton sits at 1.4547, 1.4491 and 1.4343 eV (the QCSE: 20 meV down), bound by
    7.90, 7.41 and 6.65 meV, and broadened 3.0, 4.5 and 6.0 meV.
  - Δn at 1.44 eV changes with the field.
  - About 3 s, once each exciton's search was cut to what its flat least needs: γ to
    1e-3 and ln λ to 1e-5, from 1,800 evaluations to 390.

## Found along the way

- **The bound continuum's rule** dropped a tilted well's own first states (see "The bulk
  continuum"). Now each state is judged by its own well's barriers.

- **The exciton line was half as strong as it should be** (the MQW calibration round).
  Fixed, confirmed by Elliott's limit and the measured radiative decay; the earlier
  write-up has a note.
- **The symmetrized operator order made a spurious band** in InGaAs/InP. Fixed by Burt and
  Foreman's order; tested against an independent reference.
- **The bulk continuum's gap moved with the field.** It came from the band edges' extremes
  over the line, and its own test caught it. It is now the line's end point's own gap.
- **One vector's Krylov space misses degenerate states.** At one averaged direction,
  where four states share an energy, one went missing and that spectrum was 3e-5 off. The
  deflated check search now finds such states.
- **The k·p holes counted by trapezoids** were 31 µeV off in their Fermi level for a
  parabolic band. Each interval is now integrated exactly.
- **A central-difference reference is no reference across a step.** The first check of
  Burt and Foreman's order used one, and its eigenvalues wandered with h (spurious
  odd-even modes). Linear elements replaced it.

## Tests (`test_fem_quantum`)

- **`kpAgainstLuttingerKohn`:** the 4x4 against Luttinger and Kohn's in the |3/2, m⟩
  basis (finite differences, one material, Richardson's of two grids).
  - [100] to 1.2e-6 eV; the 6x6 with a split-off 100 eV away to 1.1e-4 of it;
  - [110], axial and averaged against R's and S's complex forms and four Gauss directions,
    to 1.5–3.6e-7 eV. Against one grid the reference's own error had been 3–6e-5 eV.
- **`kpBurtForeman`:** Burt and Foreman's order across interfaces.
  - Against linear elements to 8e-7 eV; symmetrized, 67 meV apart;
  - In0.53Ga0.47As/InP's 6x6 to 2.6 nm⁻¹ with no band above its k = 0 top.
- **`kpSpectrumUnmixed`:** γ2 = γ3 = 0 (the heavy and light holes one mass).
  - The k·p spectrum is the parabolic one's to 6e-10 (the tail past the grid included);
  - with excitons to 1.5e-7 of the peak;
  - averaged over directions to 6e-10, the 15° direction's four-fold states all found;
  - with 3e18 cm⁻³, the holes' Fermi level the parabolic one's to 1 µeV, the gain to
    1.6e-5 of its most.
- **`kpGain`:** a GaAs-like 4x4 well's unbroadened gain crosses 0 at ΔF; `kpoints` 41
  gives 41 values of k.
- **`strainedWellsGain`:** compressed and stretched InGaAs on InP. TE and TM each where
  they belong; the k·p gain below the parabolic.
- **`spontaneousEmission`:** emission against absorption, parabolic and k·p, both
  polarizations' coefficients.
- **`kramersKronig`:** against the analytic α's principal value, and the window's warning.
- **`broadeningWithTheField`:** `qw.Fw` the uniform field's; the half width grown by it.
- **`aBoundContinuum`:** the well's TM over its barriers' bulk; a barrier slab's bulk
  absorption at 0 and 100 kV/cm; at 100 kV/cm a 10 nm well's e1-hh1 and its exciton
  kept.
- **`theExamples`:** the two new examples, as above.
- **`excitonsIn2D`:** Elliott's 8 Eb, approached as the well thins.
- **`excitonsAgainstKhramtsov`:** the published binding and measured radiative decay,
  both models.
- **`at77K`:** gaps, states, statistics, gain, emission at 77 K against 300 K.
- **`excitons`:** last round's checks, now with `"excitonmodel": "bastard"` and the line at
  4/(πλ²).

In Release, `test_fem_quantum` runs its 29 functions in 48 s.

Under the sanitizers (a Debug build with ASan and UBSan) it runs in 697 s, all 29
passing, with no sanitizer reports; before this round it ran in 214 s.

- **Why slower:** Eigen's complex sparse solves, unoptimized, are hundreds of times slower
  there than in Release.
- **What is lighter there:** the k·p tests use a 0.25 nm line, 41 values of k, and fewer
  states and energies. Only the strained wells' compressed case runs. The k·p laser and
  modulator examples do not run; their paths are the other tests'.
- **Limits:** each function is under 130 s against QtTest's 300 s. The test's ctest limit
  is raised to 1800 s, as the semiconductor's is.

`test_multiphysics_doc` runs in 265 s under the sanitizers, 40 passing, with no reports.

## Breaks

Each change was broken in turn, rebuilt, and its tests run: 40 breaks.

| Of | Breaks |
|---|---|
| k·p bands | the states not each kind's (hh, lh, so); the axial approximation's R with γ2; [110] taken along [100]; the average's directions 0 to 90°; no X-Y coupling; no Y-Z coupling; symmetrized, not Burt and Foreman's order; the Z-X element not the X-Z's adjoint; the bands' average not weighed; a direction's transitions not weighed; a direction's holes not weighed; an overlap's load not the shape function's; `inplane` not read |
| The eigenvalue search | one search, no check of the rest |
| The k·p spectrum | the holes by trapezoids; one cell per k interval; unbroadened, the interval's middle; no tail past the k grid; `kpoints` not read |
| Emission | `qw.gspTE` the TM emission; the emission not averaged over directions |
| Kramers–Kronig | its principal value's analytic part left out; c ħ/2π |
| Broadening with the field | not grown; the field's slope wrong |
| The bulk continuum | its gap the edges' least; "bound" keeping the box's states; the bulk per its own thickness; judged by the line's far ends; a slab of one gap confining |
| Temperature | kT 300 K's always |
| Excitons | Bastard's, not two parameters; \|F(0)\|² not normalized; no kinetic energy along the axis; the in-plane kinetic energy's c² E1 left out; the Coulomb term not γ's; the parabolic line one spin pair; the k·p line both pairs twice; `excitonmodel` not read; the Coulomb table bilinear |

One first passed: **the Coulomb table bilinear.** The binding and the line moved by less
than the tests allowed, because the energy is flat at its minimum. But γ landed on the
table's nodes in all five wells (0.600, 0.575, 0.600, 0.625, 0.675). The Khramtsov test
now asks that at least three of the five γ fall between the nodes (bicubic: 0.603, 0.569,
0.631 and 0.679 do). Caught now.

All 40 are caught.

## The whole

- **ctest:** the full ctest passes, 116 tests. One earlier run failed
  `test_diagram_tools::crossProbing`, which this round does not touch: it fails about
  one run in three alone, and it failed the same way before this round. It is offered
  as a task of its own.
- **MCP scenarios:** the script's 97 checks pass.
- **Nothing outside:** no run wrote to the workspace, the caches or the Trash.
