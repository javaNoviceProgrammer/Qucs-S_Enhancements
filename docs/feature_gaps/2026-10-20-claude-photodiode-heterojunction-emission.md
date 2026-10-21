# A photodiode under strong light, 4: emission and tunnelling across heterojunctions

The seventh of the nine missing pieces of a photodiode's physics (`2026-10-20-claude-photodiode-circuit-and-slices.md` lists them).

**Before:** an abrupt heterojunction was drift and diffusion's. Each side's carriers sat on their own bands up to the interface, and their quasi-Fermi levels were continuous across it. The interface itself had no resistance: any current crossed the step, however few carriers stood at its top.

That misses what limits a step's current, the carriers arriving at it thermally. An InGaAs absorber's holes at its InP or InGaAsP step, for instance, pile up under strong light. Their charge screens the field and the bandwidth falls.

**Now:** the semiconductor's **Heterojunctions** has two more choices:

| Choice | The step |
|---|---|
| Abrupt | As before: drift and diffusion, the quasi-Fermi levels continuous |
| Thermionic | Abrupt; its carriers cross no faster than thermal emission brings them, q v_R n′ (n′ at the step's top) |
| Tunnelling | Thermionic, and those beneath the top tunnel through its spike: (1 + δ) times |
| Graded over an element | As before |

**How it is solved:**
- **The velocity.** v_R = √(kT/2π m*), the Richardson velocity. m* is the barrier side's density-of-states mass, from its own Nc (or Nv for holes): Nc = 2 (2π m* kT/h²)^(3/2).
- **Each edge from an interface node into the other side** carries the emission G = q v_R (1 + δ) A in series at its interface end. A is the edge's face projected across the interface, A_face |t·n|: an edge at an angle to the interface carries only that share of the current across it.
- **In series with Scharfetter and Gummel's flux:** the edge's flux is multiplied by G/(G + w B), w its own conductance and B its Bernoulli toward the interface end at its step d. This is exact for a series element at that end of an edge. Without B, the depletion layer's field made the interface 5 % too conductive.
- **Held a Newton step.** The factor follows the solution as it is solved, as saturated mobilities do. Its own change is left out of the Jacobian and out of a frequency step's admittance.
- **Tunnelling:** δ = (1/kT) ∫₀^ΔE e^(u/kT) T(u) du, with T(u) = e^(−4√(2m*) u^(3/2)/3qħF) a triangle's WKB transmission u below the top. F is the field in the barrier's triangles at the edge (Grinberg et al.'s, for a heterojunction bipolar transistor's emitter).

So the interface adds Vt/(q v_R n′) per area to the device's resistance in linear response, the same whatever the mesh once it resolves the step's depletion. An edge along the interface crosses nothing and carries no emission.

**Not included:** the step's emission in a lattice temperature's bands (its step and masses are T's), and the factor's own change in the Jacobian and the small signal.

## Checks

The test is `test_fem_photodiode_power`: n-GaAs | n-Al0.3Ga0.7As, 10¹⁸ cm⁻³ each, 40 nm each side (the barrier's depletion some 17 nm), 1 mV across.

| Check | Result |
|---|---|
| Thermionic against abrupt: the interface's resistance | 0.6134 Ω more, against Vt/(q v_R n′ A) = 0.6017 Ω (v_R 8.87·10⁴ m/s from the barrier's Nc 6.99·10²³ /m³, m* 0.092 m0; n′ 6.0·10²⁰ /m³), 1.9 % |
| The same as the mesh is halved | 0.25 and 0.5 nm 0.7 % apart. Earlier, at 100 nm each side: 0.183, 0.170, 0.166, 0.164 Ω at 2, 1, 0.5 and 0.25 nm, the first coarser than the Debye length |
| Tunnelling: the interface's resistance | Divided by 1 + δ, δ 15.0 (the field 2.2·10⁷ V/m, the step 0.259 eV), as the test's own integral gives, to 0.3 % |

Before the face was projected across the interface, and before B, the interface was 0.11 Ω against 0.15 and 21 % apart between meshes.

Under the sanitizers the 0.25 nm mesh took past QtTest's 300 s: there the meshes are 0.5 and 1 nm, 2.6 % apart, checked to 3 % (60 s).

## Break checks

| Break | Caught by |
|---|---|
| The face not projected across the interface | `emittedOverAHeterojunction`: 0.537 against 0.602 Ω |
| The emission in series without the edge's Bernoulli | 0.569 against 0.602 Ω |
| The Bernoulli toward the wrong end | 0.527 against 0.602 Ω |
| The electrons' Richardson velocity of the holes' mass | 1.305 against 0.602 Ω |
| No tunnelling | the tunnelling's interface 0.613 against 0.038 Ω |
| The tunnelling mass the holes' | 0.159 against 0.038 Ω |

An edge along the interface given the emission too changed nothing: its face has no share across.

## Next

Four of the nine remain:
- hot electrons (energy balance, velocity overshoot, the transferred electrons' negative mobility);
- distortion (harmonics, compression);
- noise (shot, thermal, an APD's excess);
- the APD's convergence wall past −22.7 V.
