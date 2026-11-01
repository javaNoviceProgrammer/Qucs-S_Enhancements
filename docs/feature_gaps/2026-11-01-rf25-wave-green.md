# RF 2.5D, phase C: the retarded Green's functions

Phase B ended with the quasi-static extraction whole: C and C0 from the charge, L from C0, R by Wheeler's
rule, G, the S parameters of a line below its first resonance, and an inductor's L by partial inductances.
Phase C is the full-wave solve - "the mixed-potential integral equation: the spectral Green's functions
with DCIM and the Sommerfeld fallback, RWG and rooftops, via attachment modes, the dense fill and solve,
the currents" - and the user went to it straight ("Straight to phase C", 1 November). This is its first
round, and it is the piece the proposal names as the heart of the work and the risk: **the Green's
functions of the stack at a frequency**, the vector potential of a horizontal current and the scalar
potential of its charge, which every matrix entry of the solve will be made of. Nothing in the tab
changes yet; the next round is the solve that uses them.

## The proposal's open decisions, as taken

The proposal left phase C three questions. With no answer to them, this round takes the defaults it
proposed, and says so:

- **A lossy silicon substrate from day one?** Yes, and for nothing: each layer's permittivity is complex
  - its loss tangent and, for a substrate, its conductivity in the imaginary part - so a conductive
  substrate is a stack like any other. A test holds it to what the physics says (below).
- **Rooftops or triangles?** The mesh's own rectangles. Its cells share whole edges (phase B made it so),
  and a current from one cell to its neighbour across their shared edge is the rooftop; the inductance of
  phase B already solves for exactly those runs. Triangles come only where a layout is not Manhattan.
- **Validation targets?** Here, the closed forms that exist (below). For the solve: a microstrip's S
  parameters against the quasi-static ones at low frequency and its phase against beta l, a stub's
  resonance, reciprocity and passivity - and measured structures when they are named.

## The kernels (`rf25/rf25_wave.{h,cpp}`)

For horizontal currents on horizontal sheets the mixed-potential equation needs two kernels (Michalski and
Zheng's formulation C): the vector potential `G_A` of a horizontal current element - the same for x and y,
with no cross term - and the scalar potential `K_phi` of a point charge. Each is a Sommerfeld integral,
`G(rho) = 1/(2 pi) Int Gt(kr) J0(kr rho) kr dkr`, of a spectral function made of the stack's two
equivalent transmission lines, TE and TM - the voltage at the field's height of a unit current at the
source's:

```
Gt_A   = V_h / (j w)
Kt_phi = (j w / kr^2) (V_e - V_h)        Z_e = kz/(w eps),  Z_h = w mu0/kz,  kz = sqrt(k^2 - kr^2), Im kz <= 0
```

with a ground plane a short and the half-spaces matched. At the source the line goes both ways, the wave
going up `Z (1 + Gd) / (2 (1 - Gu Gd))` and the one going down `Z (1 + Gu) / (...)`, so nothing is divided
by anything that vanishes but at the Green's function's own poles; from there the voltage is carried slab
by slab, every exponential decaying. Before a line of code, both kernels were reduced by hand for a strip
on a grounded slab to Mosig's closed forms (`G_A = mu0/D_TE`, `G_V = (u0 + u1 tanh u1h)/(eps0 D_TE D_TM)`)
- which is also how the sign of the scalar kernel was settled - and they are the first test.

How the integral is done is where solvers of this kind spend their unplanned time, and where this round
did:

- **The singular part in closed form.** As kr grows each kernel tends to `c exp(-u dz)/u`,
  `u = sqrt(kr^2 - k^2)`, whose transform is `c exp(-jkR)/(2 pi R)` - Sommerfeld's identity. `c` comes from
  the interfaces at the source and the field: a charge on a dielectric interface has `1/(eps_a + eps_b)`,
  carried across the interfaces between by `(1 + r)` each. `k` is chosen to match the `1/kr^3` term as
  well: on an interface between two media, the harmonic mean of their `k^2` for the scalar and the
  arithmetic mean for the vector - worked out from the two-half-space kernel's expansion - which leaves a
  remainder falling as `1/kr^5`. The vector potential's image in a ground goes out the same way. A method
  of moments then integrates these over its cells in closed form, and the rest is smooth.
- **A path off the real axis.** The surface-wave poles and the branch points lie on the real kr axis -
  just below it with loss. The remainder is integrated from nought to beyond all of them (`1.5 k_max`) on
  a half-ellipse in the upper half-plane, where nothing is near; its height is kept under `1/rho`, since
  J0 grows as `exp(b rho)` off the axis, but not so low as to graze the poles.
- **The tail.** From there, panels doubling in length while J0 hardly turns; where it does, J0 is split
  into Hankel's two functions and each half turned onto a vertical line from the last panel's end - the
  first up, the second down - where each decays as `exp(-t rho)` however slowly the integrand dies along
  the real axis. Every pole and branch point is to the left, so nothing lies between.
- **A cap.** Past `1e4 k` the remainder is the kernel less its closed form, two numbers that agree to
  `(k/kr)^4`, and what is left of their difference is rounding: the integral stops there.
- **A table over distance**, from nought and then forty a decade until the step reaches a sixtieth of the
  shortest wavelength, read by a quintic of `rho` times the remainder. Built when first read, about 1 ms
  a distance - some 0.2 s for a pair of heights at a frequency - and `prepare()` builds it before a solve
  hands it to its threads.

Bessel's `J0` and Hankel's `H0` of a complex argument are their own (libc++ has neither): power series
below `|z| = 12`, Hankel's asymptotic form above.

## What it agrees with

| Case | Reference | Measured |
|---|---|---|
| `J0` at twelve complex points, both sides of `|z| = 12` | its periodic integral by the trapezoidal rule, which converges faster than any power and rests on neither series | 2e-11 |
| `H0` of both kinds | its integral `(2/(j pi)) Int exp(jz cosh t) dt`, and `J0 +- j Y0` at Y0's tabled values | 1e-9, 1e-12 |
| The spectral kernels of a strip on a grounded slab, lossless and with tan delta 0.02, at points on the ellipse and the real axis | Mosig's closed forms | 1e-10 |
| One medium over a ground, at 1, 10 and 30 GHz, two heights, 1 µm to 4.6 mm | the source and its image, each a spherical wave - exact | 9e-9 of the parts (scalar), 3e-16 (vector, all closed form) |
| A metal on 100 µm of 9.8 under 3 µm of oxide, and across the oxide, at 1 MHz | phase B's static Green's function (its images) and the static `mu0/(4 pi)(1/R - 1/R')` | 6.4e-8 and 5e-12 of the singular part |
| The same stack, lossy, at 20 GHz, the two metals swapped | itself: reciprocity | 1e-8 |
| A thick lossless slab at 30 GHz, poles on the axis | itself on a low ellipse and a high one | 1e-9 |
| The table, between its nodes, at 40 GHz | the integral there | 5.5e-9 |
| Silicon of 10 S/m under 2 µm of oxide, at 100 MHz | the oxide on a ground plane: such silicon relaxes a charge in 1e-11 s | 1.4e-5 and 1.1e-4 in size at 0.5 and 2 µm |

The measures are against the parts a method of moments adds, not the total: over a ground the whole falls
as a dipole's `1/rho^3`, at a millimetre it is 0.15 % of its own direct term, and a part in 1e8 of the
parts is a part in 1e5 of that.

## What the tests found, and what it changed

- **One panel from the ellipse to the first zero of J0 missed what is near the ellipse's end.** A ground's
  image came out a part in a thousand wrong from 160 µm on - exactly where the oscillatory branch took
  over. The branch points sit half a k below the ellipse's end, and the panel was thousands of times
  longer than that. Panels doubling out from there first: 1e-9.
- **An extrapolation of J0's half-periods ran out of terms.** With an oxide 3 µm thick the remainder dies
  only as `exp(-kr 6 µm)`, over thousands of half-periods at a millimetre, and Wynn's epsilon algorithm on
  their sums stopped a part in 1e5 short. Hankel's two halves on vertical lines replaced it, and it is
  gone from the code.
- **The doubling never stopped at nought distance**, and ran to infinity: past some kr the remainder is
  rounding, which does not decay. The cap.
- **The first spectral comparison was 2e-10 out, and the test was wrong.** It took `k0 = w/c`; the solver
  `k^2 = w^2 mu0 eps0`; and CODATA's `eps0` with `mu0 = 4 pi 1e-7` makes `mu0 eps0 c^2` one less 5.5e-10.
  The tests take `mu0 eps0` too.
- **A height a rounding step off its boundary was in no slab**: a metal at 302 µm, the stack's top summed
  as 300 + 2. Heights within a hair of a boundary are on it.
- **The silicon's phase was not an error.** Its image charge falls short of a ground's by `2 eps_ox w /
  sigma`, 4.3e-3 at 100 MHz, and that shows as a phase lag as far as the image counts - 6.6e-4 rad at
  0.5 µm, 4.3e-3 at 2 µm where the charge and its image nearly cancel. The test holds the size and bounds
  the lag by that, rather than the complex ratio by a size.

## Tested

| Test | What it holds to |
|---|---|
| `theBesselFunctionOfAComplexArgumentIsJ0` | Twelve points against the periodic integral; the first zero; I0(2); phase B's real J0 out to 60; even and real on the real axis |
| `hankelsFunctionsAreH0` | Six points in the upper half-plane against the integral, and their conjugates for the second kind; four of Y0's tabled values |
| `theSpectralKernelsAreThoseOfAGroundedSlab` | Mosig's two closed forms at six kr, lossless and lossy; the singular terms the interface's own, and the vector's image at twice the height |
| `overAGroundInOneMediumTheyAreTheSourceAndItsImage` | The source and its image at three frequencies, two heights, five distances; and nothing of the vector potential left to integrate |
| `atLowFrequencyTheyAreTheStaticOnes` | Phase B's static potential at one metal and across two, and the static vector potential, at 1 MHz |
| `theyAreReciprocal` | A lossy two-metal stack at 20 GHz, the metals swapped, by the integral |
| `thePathOffTheAxisMakesNoDifference` | A low ellipse and a high one at four distances, nought among them |
| `theTableIsTheIntegral` | Six distances off the nodes, two pairs of heights, at 40 GHz; past its end the integral itself |
| `aConductiveSubstrateHoldsChargeLikeAGround` | Silicon at 100 MHz a ground in size within 3e-4, its lag below 1e-2 and growing with the image's share; at 10 GHz no ground; without its conductivity, no ground |
| `whatItCannotAnswerIsSaid` | No frequency, NaN, a height in no slab, a metal on the ground, no stack |

## Broken on purpose

`docs/feature_gaps/2026-11-01-rf25-wave-green/breaks.py` holds fifteen: kz on the principal branch; the
scalar kernel's sign; the ground open for TE; the source's current not split; the substrate's conductivity
left out; a loss tangent that gains; the singular term blind to the interface; the vector image left to the
integral; the path grazing the axis; one long panel past the ellipse; no cap; the tail's Hankel functions
of the wrong kinds; the table read by a cubic; a height off its boundary; J0 by Hankel's form in the left
half-plane.

## What the breaks taught

- **Thirteen were caught at once; two came back marked uncaught, and neither was a hole.** The path grazing
  the axis was caught by the image test, but the table's test and reciprocity's were listed against it as
  well, and each compares the path with itself - both sides graze alike. And the height a rounding step
  off its boundary went unexercised: after the probe found it, the silicon test had been written with the
  stack's exact top. It now also takes the metal at 302 µm as a model writes it, and the break is caught.
  Fifteen of fifteen.
- The full ctest (147), the scenario script (97 checks) and both lints were green. Under the sanitizers the
  new suite takes some 40 s, no function more than 11.

## Not done

- **The solve.** These kernels are the next round's matter: the runs of current between cells, a partial
  inductance and a potential coefficient of each pair from these kernels in place of the static ones, a
  dense complex solve, ports, and S parameters held to phase B's at low frequency.
- **Vertical currents** - a via's - which want the kernels' zz and zx terms, not built.
- **The discrete complex image method**, which the proposal names beside the integral: with the table the
  integral is fast enough (1 ms a distance), so the fitting, and its silent failures, are not needed yet.
- **A metal lid over the stack**, still refused, as in phase B.
