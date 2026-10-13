# Proposal: wurtzite quantum wells (GaN, AlGaN, InGaN)

*13 October 2026 - after Qucs-S 26.1.8. A written proposal only. The review of 12 October,
"MQW multiphysics: feature gap against Ansys Lumerical MQW", lists wurtzite among the
gaps: "Zinc blende only (no piezo or spontaneous polarization)". In that round the user
asked for everything but wurtzite to be built, and for wurtzite to be a written proposal.
Nothing here is built. The facts about this tree were checked in it on 13 October 2026.
Sizes and weeks are estimates.*

**In short:** most of the work is already done. The k·p valence bands built in this round
are written in the X, Y, Z orbitals times spin, with Burt and Foreman's operator order,
and their spectrum, excitons, emission and Kramers-Kronig index take any 6x6 Hamiltonian.
A wurtzite crystal changes three things:

- **the coefficients** of that Hamiltonian;
- **the materials**: new parameters and a nitride library;
- **the polarization charges**: unlike in zinc blende, they set the field in the wells.

That comes to about 1,600 lines and two to three weeks, polarization included.

## What is there now

| Part | Now (zinc blende) | What wurtzite needs |
|---|---|---|
| Materials (`fem_materials.h`, `BandParameters`) | Eg, VBO, Δ, me, γ1-γ3, Ep, a0, ac, av, b, C11, C12, εr, n. Binaries GaAs, AlAs, InAs, InP, GaP, GaSb, AlSb, InSb and their alloys, Vurgaftman 2001. The library's "Gallium nitride" is an optical and electrical entry; its own text says "its polarization not modelled". | A wurtzite record: Eg, VBO, the crystal-field split Δcr and the spin-orbit Δso (Δ1 = Δcr, Δ2 = Δ3 = Δso/3), the electron's masses along and across c, A1-A6, Ep∥ and Ep⊥, a and c, D1-D6 (with a1, a2 for the conduction band), C11, C12, C13, C33, the spontaneous polarization Psp, e31, e33, εr. GaN, AlN, InN and the alloys AlGaN, InGaN, InAlN with their bowings (Vurgaftman and Meyer 2003; Psp bowing per Bernardini and Fiorentini). |
| Strain (`fem_quantum.cpp`, the bands' setup) | Biaxial on a substrate's lattice: εxx = (a_s - a)/a, εzz = -2 C12/C11 εxx; Bir and Pikus. | The c-plane's εzz = -2 C13/C33 εxx; Bir and Pikus's wurtzite form - θε = D3 εzz + D4 (εxx + εyy), λε = D1 εzz + D2 (εxx + εyy) - on the k·p terms. |
| k·p bands (`QuantumProblem::kpBands`, this round) | X, Y, Z ⊗ spin. The k² terms are Dresselhaus, Kip and Kittel's L, M, N from γ1-γ3. k kz is split as kx N+ kz + kz N- kx. The spin-orbit term is Δ(σ·L - 1)/3. There are four in-plane modes (axial, [100], [110], an average). | The same basis and assembly, with wurtzite's coefficients (Lumerical's MQW physics page writes them out): X: L1 kx² + M1 ky² + M2 kz², Y: M1 kx² + L1 ky² + M2 kz², Z: M3 (kx² + ky²) + L2 kz², X-Y N1 kx ky, X-Z and Y-Z N2 k kz. L1 = A2 + A4 + A5, M1 = A2 + A4 - A5, M2 = A1 + A3, M3 = A2, L2 = A1, N1 = 2A5, N2 = √2 A6 (ħ²/2m0). At k = 0: Δ1 on X and Y, and the spin-orbit split into Δ2 (Lz σz) and Δ3 (Lx σx + Ly σy). The c-plane is isotropic to this order, so one in-plane direction is exact. N2 is split for Burt and Foreman's order as Lumerical's N2± are. |
| Electrons | One mass, isotropic, in the 1D Schrödinger equation. In-plane masses are averaged over \|ψ\|². | me∥ along c in the equation, me⊥ in the plane: two inputs where there is one now. |
| Optical weights | P² = 3\|Mb\|² from one Ep. TE is (3/2) Σ \|<e\|X, Y>\|², TM 3 Σ \|<e\|Z>\|². | Ep⊥ with X and Y (TE), Ep∥ with Z (TM). In nitrides, TM against TE is set by the crystal field's order of the A, B and C bands (AlN's top band is Z-like: TM). |
| Field in the wells | A uniform field along the axis, or another physics' potential (`semi.V`), and a semiconductor's Schrödinger-Poisson loop (Self-consistent). | Spontaneous plus piezoelectric polarization: a bound sheet charge σ = -ΔP·n at each interface, about 1-3 MV/cm in GaN/AlGaN and InGaN/GaN wells. This is what makes wurtzite wells different. |
| Spectra, excitons, emission, Kramers-Kronig | Built this round for any k·p Hamiltonian. Excitons use the two-parameter variational envelope. | Unchanged. Nitride excitons are bound by 20-40 meV; εr's anisotropy is small (its geometric mean). |

## The polarization field

There are two routes. Both are needed, and they share the charges.

1. **In the wells physics alone.** For a stack drawn as layers, solve Poisson's 1D
   equation along the line the states already live on (its quadratic elements), with the
   bound sheet charges at the interfaces. Psp jumps, and e31 and e33 times the strain give
   the piezoelectric part. The boundary condition is chosen: zero field at the ends (a
   stack between thick contacts) or periodic (an infinite MQW).
   - For a periodic MQW this has a closed form,
     F_w = (P_b - P_w) L_b / (ε_w L_b + ε_b L_w): the first test.
   - The carriers' screening comes from the states' own densities, through the same
     Anderson-mixed loop the Schrödinger-Poisson step uses.
   - About 250 lines.
2. **Through the semiconductor physics.** Polarization sheet charges at heterointerfaces,
   as a fixed interface charge in `semi`'s Poisson equation. The wells then take `semi.V`,
   one way or self-consistently, as now: an LED's or a laser diode's bias across a
   p-i-n. About 200 lines, plus the nitride materials' transport entries (mobilities,
   lifetimes, Mg's deep acceptor: ionization near 170 meV).

## Steps

1. **Materials** (about 350 lines with tests): the wurtzite record and GaN, AlN, InN and
   their alloys. A `crystal` kind on the compound selects the k·p form. The tests check
   Vurgaftman and Meyer's tables and the bowed gaps of In0.2GaN and Al0.3GaN against their
   published values.
2. **k·p coefficients and strain** (about 250 lines): the wurtzite terms in
   `kpBands`, with D1-D6 on them.
3. **Polarization** (about 450 lines with both routes).
4. **Anisotropic electrons and optical weights** (about 100 lines).
5. **Tests and documentation** (about 450 lines).

## Checks against what is known

- **Bulk:** the 6x6 at k along c and across it against Chuang and Chang's closed forms of
  the A, B and C bands (Phys. Rev. B 54, 2491 (1996)). In a box of one material they must
  agree to the elements' error, as the zinc-blende 4x4 agrees with Luttinger and Kohn's
  here (1.2e-6 eV).
- **Interfaces:** Burt and Foreman's order against linear elements written in the test,
  as this round's zinc-blende check (8e-7 eV). A GaN well in AlGaN must have no band in
  the gap.
- **Polarization:** the periodic MQW's closed form above. Then GaN/Al0.2Ga0.8N wells
  against the published field of about 1 MV/cm per 0.1 of aluminium.
- **Published wells:**
  - Lumerical's own 6x6 nitride check: three 6 nm GaN wells between 10 nm Al0.2Ga0.8N
    barriers. The KB says it was compared with published results; the same stack is
    rebuilt here.
  - The QCSE's redshift of InGaN/GaN and GaN/AlGaN wells against their width (Fiorentini
    et al., Phys. Rev. B 60, 8849 (1999); measured PL).
  - InGaN well gain against Park and Chuang's.

## Risks

- **Parameters:** nitride parameters are spread more widely than arsenides' and
  phosphides'. InN's gap (0.7 against the old 1.9 eV) and InGaN's bowing vary by source.
  The library should name its sources, and a material's own value should override
  them, as for arsenides now.
- **Interface states:** the spurious-solution problem that Burt and Foreman's order fixed
  here for InGaAs/InP is worse with AlN's parameters. Its interface test must go to large
  k (3 nm⁻¹ and more) with AlN barriers.
- **Screening at high density:** in LEDs and lasers the polarization field is screened
  by the carriers. Without the self-consistent loop the transition energies are wrong
  by tens of meV. The first route must ship with its loop, not after it.
