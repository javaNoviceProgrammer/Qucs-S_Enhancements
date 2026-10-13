# Feature gaps: the MQW calibration

*13 October 2026 — on `30cd8d3`.*

The reviewer calibrated the MQW multiphysics against analytic and reference cases:
10 small models in `~/QucsWorkspace/mqw_calibration`, each compared with an
independent calculation (BenDaniel-Duke finite differences, Airy, Kronig-Penney,
Vurgaftman and Chuang's strain, Chuang's 2D absorption, Poisson-Boltzmann, slab
dispersion). 81 of 83 checks passed: the solvers were accurate, and the two failures
were material data. The review listed:

- **A:** one bug;
- **B:** the material data, and excitons;
- **C:** three things to document;
- **D:** six points of tool friction.

All of it is done here, in two commits: `fda93ed` (A, B and C: the materials, the excitons
and the notes) and `7bbf088` (D: the modes and the tools). Each item was first reproduced: the bug and the gaps in the
solver's tests, the tool items through `qucs-s --mcp-server` with HOME, settings, trash
and cache isolated.

## A. One material, two band gaps

| Review | Now |
|---|---|
| A1. With `{"material": "Indium phosphide"}`, `semi` used Eg = 1.344 eV (the library entry, through `materialProperty`), while `qw` used 1.353 eV (Vurgaftman's 300 K value): its lookup read the material's own text, then the compound, never the library. Every library material with both explicit properties and a compound was affected. Fix: make `qw` use the library before the compound. | **One order in every physics:** the material's own property, else its library entry's, else its compound's. The library's InP is 1.344000 eV in both physics. A gap given (own, library or from a gap wavelength) is the gap at 300 K. With a compound, quantum wells at another temperature move it as the compound's moves, so the library's InP at 77 K is 1.405709 eV (Varshni's change added). Before, a library value would have frozen it, or skipping the library would have ignored it. |

## B. Material data

| Review | Now |
|---|---|
| B1. Vurgaftman's 300 K gaps are 9–16 meV off for the InP family: InP +8.9 meV, In0.53Ga0.47As −16.3, In0.52Al0.48As −13, Q1.3 +8; Chen's MQW e1–hh1 15 meV above its PL. Proposed: calibrate the bowings to 300 K data, or offer a "300 K measured" set beside Vurgaftman's. | **A set fitted to 300 K measurements, beside Vurgaftman's:** `"bands": "measured"` on a material, *Band gaps* in its settings. Only the gaps differ: the masses, offsets, lattice constants and deformation potentials are Vurgaftman's either way. The default stays Vurgaftman's, so existing models are unchanged. The fit's values are in the table below. |
| B1. A material option for a target gap or PL wavelength that adjusts Eg while keeping the masses and deformation potentials. | **A gap wavelength,** `lambdag`: Eg = hc/λg when Eg is not given. A quaternary named by its gap ("1.3Q") is `"lambdag": "1.3[um]"`, 0.953725 eV in both physics, its masses and strain still its compound's. A well's PL sits above its material's gap by the confinement; the spectrum step's `qw.Ex` (below) is what a PL peak is compared with. |
| B1. Document that a material's own Eg/VBO overrides the compound in `qw`: the calibration knob. | **Said where it is set:** in the Eg property's help, the compound's, and the format text and README. A given gap moves the conduction band, and the valence band stays at the compound's VBO unless VBO is given too. |
| B2. No excitons, so measured QCSE is out of reach. | **Excitons in the spectrum step** (`"excitons": true`). See below. |

The fitted set at 300 K:

| Material | Vurgaftman's | The fit | Measured |
|---|---|---|---|
| InP | 1.3529 eV | 1.3440 | 1.344 |
| GaAs | 1.4225 | 1.4240 | 1.424 |
| In0.53Ga0.47As | 0.7373 | 0.7498 | 0.750 (Goetz et al.'s bowing, 0.43) |
| In0.52Al0.48As | 1.4507 | 1.4601 | 1.45–1.47 |
| In0.72Ga0.28As0.61P0.39 (Q1.3) | 0.9630 | 0.9506 | 0.9555 (Nahory et al.) |
| Al0.165Ga0.305In0.53As (1.3Q) | 0.9935 | 0.9540 | 0.954 |

How the fit is made:

- **InP, GaAs and InAs:** each binary's Varshni curve is shifted to its gap measured at
  300 K.
- **InGaAs and InAlAs:** their bowings are refitted.
- **InGaAsP:** its own correction has two terms, (1 − x) y (1 − y)(0.131 + 0.499 y). It
  keeps the lattice-matched line within 7 meV of Nahory's 1.35 − 0.72 y + 0.12 y²; one
  term could not do better than 21 meV.
- **AlGaInAs:** its own bowing is 1.815 xyz. The lattice-matched line stays within
  15 meV of Olego et al.'s 0.76 + 0.49 z + 0.20 z².

### Excitons (B2)

The spectrum step's `"excitons": true` adds, for each transition in 1D wells:

- **The 1s exciton's binding:** Bastard's variational envelope χe(ze) χh(zh) √(2/π)/λ
  e^(−ρ/λ) over the states' own envelopes. A field pulling them apart weakens it, which is
  the QCSE's exciton.
- **Its line,** of strength |F(0)|² = 2/(πλ²).
- **The continuum above it,** raised by the 2D Sommerfeld factor 2/(1 + e^(−2π√(R/ε))),
  with R = Eb/4.

Details:

- **Permittivity:** the step's `epsr`, else the wells' material's (its `epsilonr`, else its
  compound's; a Quantum Well Material's own, else nr²).
- **Globals:** `qw.Eb`, `qw.Ex` and `qw.aX` give the e1-hh1 exciton's binding, line and
  radius.
- **Line shapes:** a choice of Lorentzian, Gaussian or hyperbolic secant, the linewidth
  each one's half width at half maximum. A Lorentzian's tails absorb far below the edge,
  which matters for a modulator's loss at 0 V.
- **With carriers:** the excitons are screened, left out, and the log says so.

How the binding is computed:

1. The two |χ|² are binned along the axis, and the distribution of their separation P(s)
   is formed.
2. The Coulomb term is q²/(4πε) ∫ P(s) g(s/λ)/λ ds, with g(u) = 4 ∫ t e^(−2t)/√(t² + u²)
   dt tabulated once.
3. The energy is minimised over λ by golden section.

**Checks:**

- **A 10 nm infinite well** (me 0.067, the heavy hole's in-plane mass 0.11, εr 12.9): bound
  by 8.3159 meV and 12.6024 nm across. The exact variational integral gives 8.31602 meV and
  12.60235 nm: G in Struve's and Bessel's functions, SciPy's quadrature to 1e-10.
- **The test's first reference** (Simpson's rule on G) was 0.28 % off, missing G's steep
  small-s part. The code's table was right.
- **Field:** at 100 kV/cm it is bound by 7.7573 meV, its line 10.19 meV lower.

## C. Documented

| Review | Now |
|---|---|
| C1. Absorption uses each state's probability-weighted in-plane mass (`e.mPar`, `h.mPar`), not the well's: a reference with the well's masses was 0.75 % low. | Said in the spectrum step's description, the format text and the README: each transition's reduced mass is of its two states' own in-plane masses, each the inverse mass averaged over its \|ψ\|². |
| C2. `zero` is a hard wall: a 5 nm well's e2, 2 meV below the barrier top, moved 0.1 meV with 30 nm barriers. Note it, or offer an open boundary. | Noted on the Zero Wavefunction feature and in the format text, with the reviewer's example: a state within a few meV of the barriers' top reaches the wall and is raised by it, so such models need thicker barriers. An open (decaying) boundary would make the eigenproblem nonlinear in E; not done. |
| C3. h1d accuracy: quadratic elements, error ∝ h⁴; 0.5 nm is fine for stacks of hundreds of nm. | The h1d help says it: the error goes as h⁴, 0.1 nm finds a 10 nm well's first states to 1 µeV, 0.5 nm to about 0.2 meV. It also says that a plain number is metres (the Chen note's D3). |

## D. Tools

| Review | Now |
|---|---|
| D1. A plot group whose tag equals a study's (`conv`) is silently renamed: `fem_plot plot=conv` said "there is no plot group conv". | **Said, both times.** Reading the model notes the rename, and `fem_describe` returns it as `read with notes`: "Levels's tag conv is another node's already: it is pg1d1 here". Asking for `conv` now answers "conv is Study, not a plot group (the plot groups: pg1d1 (Levels), …)". A file whose tags clash still opens: refusing it would lose models written by hand. |
| D2. After `fem_plot` the data display is in front, so the next call without `path` failed: "2 models are open: which?". A batch of 13 calls lost 4. | **Without `path`:** the model in front; else the model whose plot group's display is in front (`<model>_<tag>.dpl` beside it); else the model a tool last acted on, if still open. Only then is the question asked, and it now names the open models. |
| D3. `fem_evaluate all=true` on a states study repeated every global once per state, and `mode: 1` did not filter it: 80 values to read 20 numbers. | **Once per point,** with a note: a global the same at each state of a point (`qw.Ee1`) is given once for it. One that differs (`qw.E_e`) is still given at each. With `all`, `mode` (or the new `state`) takes that mode or state alone at each point. |
| D4. Γ from a cut line through a TM mode is wrong at index steps: 0.261 by line, 0.2569 by domain integrals, 0.2566 exact. A confinement derived value was wanted. | **A Confinement Factor derived value:** each mode's share of the domains chosen, of three kinds. *\|E\|²* gives the usual Γ. *Power* gives Sz's share. *Index* gives (c ε0/2) ∫ n \|E\|² there over ∫ Sz: ∂neff/∂n, a modal gain's Γ, so a Δn there moves neff by Γ Δn. Nothing is integrated along cut lines. |
| D5. A 2 µm PEC/PMC strip let a TE lateral-order mode outrank TM0 in a high-contrast slab: `modes: 1` returned 2.821 (TE01), not 2.053. Wanted: mode selection by polarization. | **By polarization:** the mode step's *Polarization*, TE or TM, keeps one alone, asking for more modes until there are enough. `emw.TEfraction` is each mode's share of its transverse \|E\|² along x. The reviewer's case: a TE mode at 2.82129 outranks TM0; asked for TM, 2.053319 against the slab's 2.053320. |
| D6. A model file edited on disk while its tab held a solution was not reloaded: each rebuild needed close, discard and reopen. | **Read again.** The cause: a solve adds default plots when a study has none in 2D, and that edit marked the model changed, so a change on disk was not loaded. The solve's own edit now leaves a model that was as saved still as saved; one the user had edited stays changed, as before. A solved model written anew on disk is read again. |

## Tests

| Item | Tests | File |
|---|---|---|
| A1, B1 | `theAlloysAt300K`: the fit against the measured gaps; Varshni's slope kept; the default Vurgaftman's; only the gaps differ | `test_fem_quantum` |
| | `oneMaterialOneGap`: the semiconductor's and the wells' gaps 20 nm into a slab - the library's InP, In0.53GaAs with the fit, a 1.3 µm gap wavelength, InP at 77 K | `test_fem_quantum` |
| B2 | `lineShapes`: the infinite well's spectrum under each shape, its steps' to 2e-6 | `test_fem_quantum` |
| | `excitons`: the binding and radius against the exact integral (to 1e-4); the 1s line's peak; the Sommerfeld factor; the light holes' line in TM; a field's QCSE; screened with carriers | `test_fem_quantum` |
| D5 | `modesByPolarization`: the reviewer's strip, a lateral TE mode at 2.82129 above TM0; asked for TM, the slab's TM0 to 1e-6; 4 µm wide (Release builds), ten TE modes above it, found all the same | `test_fem_waves` |
| D4 | `confinementFactors`: TE0 and its three Γs against the slab's closed form, Γ's relation to the index's, and a finite difference of the core's index | `test_fem_waves` |
| D1, D2, D3, D6 | `claudesToolsAfterTheCalibration`: through the tools, two models open | `test_multiphysics_doc` |

**Sanitized builds.** Every new function runs in at most 103 s, against QtTest's 300 s
(CI's runner is about 1.45 times slower). The mode solves take most of a minute each
there, so the 4 µm strip and the finite difference run in Release builds only.

## Breaks

Each change was broken in turn and its tests run: 34 breaks, all caught.

| Item | Breaks |
|---|---|
| A1 | the wells reading the material alone, not its library; a gap given not moved with the temperature |
| B1 | the gap wavelength not read; InP not moved to its 300 K gap; InGaAs's and InAlAs's bowings Vurgaftman's; InGaAsP's own bowing of one term; AlGaInAs without its own; the semiconductor's compound Vurgaftman's always; "measured" not read |
| B2 | the 1s line half as strong; the Sommerfeld factor's R the binding itself; the exciton's kinetic energy doubled; a light hole's exciton looked for among the heavy holes; the electron's envelope taken for the hole's; the step's permittivity left out; excitons kept with carriers; the Gaussian's σ its half width; the sech's cumulative a Lorentzian's |
| D1 | a renamed tag not said; the plot groups not named |
| D2 | a display's model not found; the model last acted on not taken; a path's model not remembered |
| D3 | all modes given though one is asked; a global given at each state; `state` not read |
| D4 | the index's Γ without c ε0/2; Sz's share over \|E\|²; the core taken as everything |
| D5 | the polarization not asked; the TE fraction y's; no more modes sought for a polarization |

One break first passed: **no more modes sought for a polarization.** In the 2 µm strip,
the first search's six candidates already held TM0. The 4 µm strip, with ten TE modes
above it, was added for it. The light holes' check (TM below the e1-lh1 edge) was added
before the breaks ran, since the first exciton test looked at TE alone.

Verified on the round's code:
- the full ctest passes: 116 tests;
- the sanitized `test_fem_quantum`, `test_fem_waves` and `test_multiphysics_doc` pass:
  214 s, 466 s and 263 s;
- the MCP scenario script passes: 97 checks.

## Not done

- **Excitons in wires** (2D quantum states): an error says so.
- **An open boundary for 1D states:** see C2.
- **A PL target on the wells,** solving for the material's gap: the gap wavelength sets the
  material's gap, and the exciton's line is what to compare with a PL.
