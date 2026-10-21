# Mobility models: the review of 20 October

The review (`qucs-reviews/2026-10-20-claude-mobility-models-coverage.md`) set the semiconductor physics' mobility models beside a mobility overview (Vasileska, Arizona State University: experimental mobilities, then the low-field, inversion-layer and high-field models of a commercial device simulator).

**Its verdict:** not fully supported.
- Arora's model and the saturation velocity's temperature dependence had to be typed in by hand.
- Dorkel and Leturcq's, Klaassen's, Shirahata's, Yamaguchi's and Tasch's models, the SURFMOB surface model and Hall transport were missing.

It read 914fce9. Since then (b8b3c30) a lattice temperature TL scales each element's mobility by (TL/T)^-ζ, and Quay's saturation velocities follow it.

**Now:** every model the review listed is there, except Tasch's and SURFMOB (below). There is also an expression of the state, and Hall transport at rest and in time.

**The models' equations and silicon's parameters:** the overview names most models without their equations. They come from the papers it cites, as device simulators implement them:
- the overview's reference simulator: its user manual's parameter tables (2004);
- Sandia's Xyce and Charon, DEVSIM, and Crosslight's model notes, for the equations;
- where these disagree, each choice is said below.

## Low-field bulk mobility (the Mobility choice)

| Review | Now |
|---|---|
| Constant, Caughey and Thomas by doping, Masetti | As they were |
| Caughey and Thomas by temperature: partial, the whole mobility × (T/300 K)^-ζ | Arora's has each of its terms' own temperature dependence, at each element's TL |
| Arora: by hand | **By doping and temperature (Arora)**: µmin Tn^a + µd Tn^b/(1 + (N/(Nref Tn^c))^(α Tn^d)), Tn = TL/300 K, at each element's TL. Silicon's values are Arora, Hauser and Roulston's (1982): electrons 88 and 1252 cm²/(V s), Nref 1.26e17 cm⁻³; holes 54.3 and 407, 2.35e17; exponents -0.57, -2.33 (holes -2.23), 2.4 and -0.146, α 0.88. (The reference simulator's defaults differ: Nref 1.432e17 and 2.67e17, c 2.546.) |
| Dorkel and Leturcq: no carrier-carrier scattering | **With carrier-carrier scattering (Dorkel and Leturcq)**, below |
| Klaassen: no | **Unified (Klaassen)**, below |

**Dorkel and Leturcq's model (1981).** It combines the lattice, the dopants and the carriers' scattering of each other.

| Term | Form (cm, V, s, K) |
|---|---|
| Lattice | µL (TL/300 K)^-γ: silicon's 1430 and 495 cm²/(V s), γ 2.2 |
| Dopants | µI = A T^1.5/N (ln(1 + B T²/N) - B T²/(N + B T²))⁻¹: A 4.61e17 and 1e17, B 1.52e15 and 6.25e14 |
| Carriers | µcc = 2e17 T^1.5/√(np) / ln(1 + 8.28e8 T² (np)^-⅓) |
| Combined | µL (1.025/(1 + (X/1.68)^1.43) - 0.025), X = √(6 µL (µI + µcc)/(µI µcc)) |

The carriers are each element's (its corners' mean), at each Newton step.

**Klaassen's unified model (1992).** 1/µ = 1/µL + 1/µDA:
- **The lattice:** µL = µmax Tn^-θ.
- **The rest:** µDA = µN (Nsc/Nsc,eff)(Nref/Nsc)^α + µc (n + p)/Nsc,eff, with µN = µmax²/(µmax - µmin) Tn^(3α - 1.5) and µc = µmax µmin/(µmax - µmin) Tn^-½.
- **Nsc:** what scatters the carrier: the dopants, clustered at high doping (Nd (1 + 1/(cD + (Nref,D/Nd)²))), and the other carriers.
- **Nsc,eff:** the repelling dopants screened by G(P) and the other carriers by F(P). P combines Conwell and Weisskopf's screening and Brooks and Herring's.
- **Silicon's values:** µmax 1417 and 470.5 cm²/(V s), µmin 52.2 and 44.9, θ 2.285 and 2.247, Nref 9.68e16 and 2.23e17 cm⁻³, α 0.68 and 0.719; clustering Nref,D 4e20 and Nref,A 7.2e20 cm⁻³, cD 0.21, cA 0.5; masses 1 and 1.258 m0.
- **Two choices, as device simulators make them:**
  - F's r5 is -0.8552, not the paper's -0.01552 (Xyce notes both).
  - G is held at its minimum below its P, where it would otherwise turn up (0.3246 for electrons, 0.2891 for holes at 300 K).
- **Its majority carriers' mobility is Masetti's arsenic and boron fit:** Klaassen took its parameters from Masetti.

## Inversion layers (surface scattering, at the field across the nearest interface)

| Review | Now |
|---|---|
| CVT/Lombardi: T the uniform physics T, not TL | **TL now**: the acoustic phonons' C N^λ/(TL E⊥^⅓) |
| Shirahata: no | **Surface model: Shirahata**, below |
| Yamaguchi: no | **Mobility: Inversion layers (Yamaguchi)**, below |
| Tasch: no | **Not added.** Its equations (Shin, Yeric, Tasch and Maziar 1991) could not be read reliably: the manual's text came garbled (exponents' places and signs, a term mixing cm⁻³ and cm⁻²), and the paper was not at hand. It also needs the field at the inversion layer's edge and its own derivative along the field across: a planar device's model. |
| SURFMOB: no | **Not added.** It is Watt's effective-field model in the reference simulator. Its default table was not in the parts of the manual that could be read, and its charged-impurity term reads the inversion layer's sheet charge and the bulk's doping, not a point's state. Shirahata's and Yamaguchi's cover the effective-field degradation. |

**Shirahata's model (1992).** 1/µ = 1/µbulk + D (1/µE - 1/µE(0)), with µE = µ0 (TL/300 K)^-θ / ((1 + E⊥/E1)^P1 + (E⊥/E2)^P2). The field's scattering is added to the bulk's (any bulk model), faded by D = e^(-d/lcrit) as Lombardi's is.

Silicon's values: µ0 1430 and 500 cm²/(V s), θ 2.285 and 2.247, E1 6.3e3 and 8e3 V/cm, E2 7.7e5 and 3.9e5 V/cm, P1 0.28 and 0.3, P2 2.9 and 1.

**Its form, chosen:**
- The manual's garbled text reads best as 1 + (E⊥/E1)^P1 + (E⊥/E2)^P2; Charon codes (1 + E⊥/E1)^P1 + (E⊥/E2)^P2.
- With E1 only 6.3 kV/cm, the first has an infinite slope at no field and cuts the mobility by a quarter at 63 V/m.
- The second is flat at no field and falls as E⊥^-0.28 above E1, the universal curve's phonon slope.

**Yamaguchi's model (1979, 1983).** It is used on its own, without a surface's scattering or saturation after it, at 300 K.
- **Low field:** µ0 = µL (1 + N/(N/S + Nref))^-½.
- **Across the interface:** µs = µ0 (1 + as D E⊥)^-½.
- **Along the current:** µ = µs (1 + x²/(x + G) + (µs E∥/vs)²)^-½, x = µs E∥/uL, E∥ the force along the current.
- **Silicon's values:** µL 1400 and 480 cm²/(V s), S 350 and 81, Nref 3e16 and 4e16 cm⁻³, as 1.54e-5 and 5.35e-5 cm/V, uL 4.9e6 and 2.928e6 cm/s, vs 1.036e7 and 1.2e7 cm/s, G 8.8 and 1.6.
- **The parallel field's form:** the manual's text could not settle whether G is a factor or in the denominator. It is Yamaguchi's published x²/(x + G).

## High field

| Review | Now |
|---|---|
| µ0/(1 + (µ0 E/vsat)^β)^(1/β), β 2 and 1, along the current, constant vsat | As they were |
| vsat(T) = 2.4e7/(1 + 0.8 e^(TL/600 K)) cm/s: by hand, at the uniform T | **Saturation velocities' temperature: exponential**: both carriers', at the physics' T and each element's TL (1.035e7 cm/s at 300 K, 9.38e6 at 400 K). Quay's (since b8b3c30) also follows TL. |

## Experimental mobilities

| Review | Now |
|---|---|
| Hall: R_H, r, µH: no magnetic field in drift and diffusion | **Hall transport**, below |
| µeff, µFE, µsat: post-processing | As before: extractions from the contacts' currents |

**Hall transport.** The semiconductor physics' **Magnetic flux density Bz**, out of the plane, turns each carrier's current: J = (J0 ∓ µH Bz ẑ×J0)/(1 + (µH Bz)²), electrons' minus, holes' plus. µH = r µ is the Hall mobility, r the material model's **Hall factors** (default 1; silicon's about 1.1 to 1.2 for electrons, 0.7 to 0.8 for holes). A field in the plane would turn the current out of it: nothing in 2D.

How it is solved:
- Each triangle's mobility along its own current is µ/(1 + c²), c = ∓r µ Bz: its edges' weights.
- The turned part c/(1 + c²) ẑ×J0 goes through the triangle's share of each edge's face (its signed cotangent's: a uniform current's divergence nought). J0 is fitted to the triangle's three edges' Scharfetter and Gummel currents, each at its own mobility.
- The Jacobian has its exact change. The contacts' currents and the current densities shown (semi.Jx...) include the turned part.

It is solved at rest and in time. A small signal, noise, contacts behind circuits and a device in slices are not solved in a field, and each says so.

## An expression of the state

**Mobility: An expression**, one for electrons and one for holes, of:
- N, Nd, Na (1/m³);
- T (the lattice's, K);
- n, p (1/m³);
- Epar (the force along that carrier's current, V/m);
- Eperp (the field across the nearest interface, V/m);
- dsurf (the distance to it, m);
- the model's parameters.

Each element's is evaluated at its state, held a Newton step as saturated mobilities are, and saturation or a surface's scattering may follow it.

## The review's "not checked"

| Not checked | Now |
|---|---|
| Whether expressions of T in vsatn and munmin are taken by a solve | Yes, at the physics' T: Caughey and Thomas's µmin 68.5 (T/300 K)^-½ cm²/(V s) at 350 K gives the formula's mobility to 1e-6, and a vsatn of 1e7 (300 K/T) cm/s gives the saturated mobility's (`parametersOfTemperature`). Under a lattice temperature the built-in models follow TL; these follow the physics' T. |
| Numerical agreement against a reference | See the checks below: Arora's against Masetti's fit of the same measurements; Klaassen's majority mobility against Masetti's, and its minority one; Hall's field against R_H J B. Lombardi's at the universal curve's scale, and saturation's against the force along the channel, as `aMosfetsMobility` has had. |

## Checks

The tests are in `test_fem_mobility` (new); each solves a silicon bar, a MOS capacitor or a Hall bar.

| Check | Result |
|---|---|
| Arora's at 1e15, 1e17, 1e19 cm⁻³, at 300 K and at a TL of 400 K | The formula's to 1e-6 (777.44 cm²/(V s) at 1e17 and 300 K, 513.52 at 400 K) |
| Arora's against Masetti's at 300 K | Within 10 %: electrons 2.7, 6.9 and 5.6 % apart |
| Dorkel and Leturcq's dark (1e15) and lit (n = p = 7.2e17 cm⁻³) | The formula's at the point's n, p to 1 %; lit, 207 against 1365 cm²/(V s) dark |
| Klaassen's at 1e15, 1e17, 1e19, 1e20 cm⁻³ n-type, 1e19 p-type, and lit | The formula's to 1e-4 (lit 1 %); the majority electrons Masetti's to 5 % to 1e19 (727.054 at 1e17 both) |
| Klaassen's minority electrons in p⁺ 1e19 | 205.8 cm²/(V s), against the majority's 108.0 in n⁺ 1e19 |
| Shirahata's and Lombardi's across a MOS capacitor's interface (2 V and -2 V, fields to 3.5e7 V/m), at 300 K and a TL of 350 K | Each point's mobility the formula's at its own field to 1e-7 (lcrit 1 m: e^(-d/lcrit) 1 - 1e-8) |
| Yamaguchi's low-field (1e15 to 1e19), across the interface, along a bar's current at 5 kV/cm | To 1e-6, 1e-7 and 1e-4; 0.930 of its low field's along the current |
| The exponential vsat at 300 K and a TL of 400 K, a bar at 10 V | µ0/√(1 + (µ0 E/vsat)²) at vsat 1.0349e5 and 9.3816e4 m/s to 1e-6 |
| An expression: of T at 400 K; Caughey and Thomas's saturation written in Epar; of n lit; of Eperp | 562.5 cm²/(V s) exactly; 317.21479 A against the built-in's 317.21479 A; 1 %; 2e-15 |
| A Hall bar (16 µm by 2, 1e16 cm⁻³, 0.5 T): its field across, electrons (r 1.15) and holes (r 0.7), both ways round | r J Bz/(q n) to 1e-3, 1e-5 to 1e-4 at meshes of 0.5 to 0.05 µm (its inner micrometre's voltage); electrons' top below the bottom, holes' above |
| The same bar's current | Without the field's to 4.5e-4: no magnetoresistance of one carrier in a long bar (what is left, the ends') |
| The current density across it (semi.Jy) | 7e-7 of the current along it |
| The same bar stepped on 1 ns in time | Its field across its rest's to every digit printed |
| A small signal, noise in a field | Not solved, and said |

## Found on the way

**A Hall bar's whole width is short of E W.** The insulating sides carry no field, so the Hall field falls to none within a Debye length of each (41 nm at 1e16 cm⁻³). The voltage across the whole 2 µm is 2.8 to 3.8 % short at meshes of 0.25 to 0.05 µm, against 2 L_D/W = 4.1 %. This is physical, not the discretization: the bulk's field is the formula's to 1e-5. The check reads the inner micrometre.

## Break checks

| Break | Caught by |
|---|---|
| Arora's at TL by the ζ factor, not its own | `aroraByDopingAndTemperature`: 1322 against 709 cm²/(V s) at 400 K |
| Arora's α without its exponent d | `aroraByDopingAndTemperature`: 710.25 against 709.17 |
| Dorkel and Leturcq's without the carriers' scattering | `dorkelCarriersScatterEachOther`: lit 1365 against 351 |
| Klaassen's repelling dopants unscreened (G 1) | `klaassenMajorityAndMinority` |
| Klaassen's r5 the paper's -0.01552 | `klaassenMajorityAndMinority` |
| Klaassen's without clustering | `klaassenMajorityAndMinority`: 108.086 against 108.035 at 1e19 |
| Klaassen's G not held at its minimum | `klaassenMajorityAndMinority` at 1e20: 156.5 against 159.1 |
| Shirahata's the other reading, 1 + (E⊥/E1)^P1 | `surfaceModelsAcrossTheInterface`: 29 % off |
| Lombardi's at the physics' T | `surfaceModelsAcrossTheInterface`: 8 % off at 350 K |
| Yamaguchi's G a factor | `yamaguchiAcrossAndAlong` |
| Yamaguchi's without its field across | `yamaguchiAcrossAndAlong`: 4.4 times the formula's |
| The exponential vsat not at TL | `saturationVelocityByTemperature`: 0.01835 against 0.01809 |
| An expression without Epar | `anExpressionsMobility`: 2243 against 317 A |
| Hall's turned the other way | `hallBar`: the voltage's sign |
| Hall's without 1/(1 + c²) along the current | `hallBar`: its field 1.3 % off |
| Hall's turned currents left out of the contacts' | `hallBar`: its field 0.6 % off (the current read short) |
| Hall's current density shown unturned | `hallBar`: Jy/Jx 0.08 |
| Hall's face shares unsigned | `hallBar`: its field 0.15 % off |

**Not caught at first, one:** G held at its minimum. It acts only where the screening's P falls below it, near 1e20 cm⁻³; the 1e20 case was added for it.

**Sanitized builds:** the MOS capacitor's skin is meshed at 5 nm, not 2, and the surface models' check is at 2 V and 350 K alone. Its twelve solves went past QtTest's 300 s there; the Hall bar takes 160 s, the rest 11 to 97 s each.
