# RF 2.5D, phase B: the charge on the metals, and what a line is made of

Phase A ended with a layout processed, meshed and drawn, and nothing solved on it. This is the first
piece of phase B - "the quasi-static extraction: charge MoM with the static layered Green's function for
C, partial inductances for L, R from the surface impedance - RLGC, Z0, and S-parameters below the first
resonance" - and it is the half of that sentence that stands on its own: **C, C0, and from them a line's
eps_eff, Z0, L and velocity.** R, the partial inductances, the ports and the S-parameters are not here;
the last section says so.

The proposal calls the Green's functions "the heart of the work, and the risk". That is where most of
this round went, and it is why the round is worth reading.

## The static Green's function (`rf25/rf25_green.{h,cpp}`)

The potential of a unit charge in a stack of dielectrics, with or without a ground plane under it. There
is no closed form in the space domain, so the spectral one is where it starts: the reflections of a
stratified medium follow the standard recursion in the transverse wavenumber `k`,

```
R_up = (r + R_up' e^-2kh) / (1 + r R_up' e^-2kh),     r = (eps_below - eps_above)/(eps_below + eps_above)
```

with `R = -1` at a ground plane, and the charge's own slab contributing

```
S(k) = [R_dn e^-2k a + R_up e^-2k b + 2 R_up R_dn e^-2kh] / (1 - R_up R_dn e^-2kh)
```

for a charge and a point both at the metal's height, `a` and `b` its distances to the slab's faces. Every
exponent is negative, so none of it can overflow however thick the stack or however large `k`.

Then the round's one real idea, which is the proposal's own ("the discrete complex image method where it
converges, and direct Sommerfeld integration as the fallback and the reference"):

- **The images.** `S(k)` is a rational function of the `e^-2kh` with constant coefficients, so expanding
  it gives a sum of exponentials - and every exponential transforms in closed form,
  `Integral e^-kz J0(k rho) dk = 1/sqrt(rho^2 + z^2)`. So the reflections *are* a set of image charges,
  and here they are not fitted, as a discrete-image method would fit them, but **enumerated**: each image
  is one path of reflections, carrying the product of the coefficients along it and the length it
  travelled. A small series arithmetic (multiply, and divide by expanding `1/(1-x)`) does the recursion
  symbolically, terms below 1e-13 are dropped, and what was dropped is reported rather than forgotten.
  The result is exact to the term it stops at, closed form in the distance, and costs nothing per
  distance asked for.
- **The integral.** The same thing from `G = 1/(2 pi) Integral Gt(k) J0(k rho) k dk`, with the free-space
  term taken out so what is left decays; panels graded in `k` near the charge and, far from it, the
  integral taken between J0's zeros and the alternating series accelerated. Slower and rougher, but it
  rests on no expansion - so it is the reference the images are held to.

`besselJ0` is the module's own, because libc++ has no `std::cyl_bessel_j` and POSIX's `j0` is not
promised on every platform the CI builds for. It is the standard split (series below 8, amplitude and
phase above), measured at 5e-9 absolute against the system's.

## The charge MoM (`rf25/rf25_static.{h,cpp}`)

Each mesh cell carries a uniform charge; the potential at a cell's centre is the sum over every cell of
its charge times a coefficient; holding one conductor at a volt and the rest at none and solving gives a
column of the capacitance matrix. Three things are worth naming:

- **Every near term is closed form, images included.** The potential of a uniformly charged rectangle has
  one - `x asinh(y/sqrt(x²+z²)) + y asinh(x/sqrt(y²+z²)) - z atan(xy/(z r))` at its four corners - and it
  holds for an image out of the plane as well as for the charge itself. So a cell's own term and its near
  neighbours' are integrated exactly over the rectangle, for the charge *and* for all of the stack's
  images, with no quadrature anywhere. Beyond four cell widths the charge is taken at the centre, where
  the difference is below a part in ten thousand.
- **C and C0 come from the same mesh.** The second solve replaces every dielectric with vacuum, which is
  what makes `eps_eff = C/C0` meaningful, and it is the same matrix structure with a different Green's
  function.
- **The relations are the engine's own.** `eps_eff = C/C0`, `Z0 = 1/(c0 sqrt(C C0))`, `L = 1/(c0^2 C0)` -
  copied from `fem_results.cpp`'s `lineparams`, so a structure solved by the FEM in cross-section and by
  this over a layout gives numbers that can be compared at all.

A capacitance over a layout is an absolute one, in farads; `lineParameters()` turns it into a per-metre
one for a length the caller gives, and the tests get that length the honest way - by solving two lengths
of the same line and taking the difference, which cancels exactly what the two ends add.

## What it agrees with

| Case | This | The reference | Apart by |
|---|---|---|---|
| Free space | `1/(4 pi eps rho)` | Coulomb | exact |
| A charge over a ground plane | images | its one image charge | < 1e-8 |
| A charge at one dielectric interface | images | the `(e1-e2)/(e1+e2)` image | < 1e-8 |
| A slab over a ground plane | images | its image series, summed out | < 2e-7 |
| The images, generally | images | the spectral integral, over five stacks | < 1e-6 |
| The images as a function of k | the series | the spectral recursion itself | < 1e-7 |
| A rectangle's near term | closed form | a 400 × 400 midpoint sum | < 1e-5 off it, 2e-3 on it |
| A plate over a ground plane | 2.17, 1.63, 1.33 × the parallel-plate value at a/h = 4, 8, 16 | the edge correction's own `~5 h/a` | the measured 4.7, 5.0, 5.3 |
| Microstrip, W/h = 1, eps_r 9.8 | eps_eff 6.496, **Z0 50.07 Ohm** | Hammerstad and Jensen: 6.620, 49.21 | **1.9 %, 1.7 %** |
| Microstrip, W/h = 3, eps_r 9.8 | eps_eff 7.248, **Z0 25.86 Ohm** | Hammerstad and Jensen: 7.368, 25.78 | **1.6 %, 0.3 %** |

Both branches of the closed form, so that one number agreeing is not mistaken for a method that works.
Hammerstad and Jensen is itself good to about a per cent, so the solver and the formula agree to within
the formula's own error at W/h = 3 and to twice it at W/h = 1.

## Tested

| Test | What it holds to |
|---|---|
| `besselJ0IsItsOwn` | J0 against the values of record at 0, 1, 2, 5, 10 and 50, its first zero, evenness and its decay |
| `theStackIsReadAsSlabs` | The metals taking no room, each dielectric keeping its thickness, the ground noted, a z on a boundary belonging to the slab below, and a stack with no dielectric refused |
| `freeSpaceIsCoulombsLaw` | Coulomb's law over six decades of distance, the permittivity of the slab the charge is in, and a charge outside the stack refused |
| `aGroundPlaneIsTheImageCharge` | The image charge, by the images to 1e-8 and by the integral to 1e-4; no potential at all on the plane itself |
| `oneInterfaceIsItsImage` | The `(e1-e2)/(e1+e2)` image, and the spectral reflection being `K e^-2kd` at every k |
| `aSlabOverGroundIsItsImageSeries` | The microstrip's own geometry against its image series summed to 4000 terms |
| `theImagesAndTheIntegralAgree` | The two methods over three stacks and six distances each, and nothing lost to truncation |
| `theImagesSumBackToTheSpectralReflection` | The images summed back up as a function of k against the spectral recursion itself, over four stacks and seven wavenumbers - the one recursion against the other, with no integral between them |
| `whatTheTruncationDroppedIsCounted` | A stack built to truncate reporting that it did, still agreeing with the integral to about what it dropped, and a mild stack dropping nothing |
| `aStackOfSlabsIsBoundedAndFallsOff` | Four slabs: a reflection coefficient never above one, the charge's own permittivity, the ground pulling the potential down |
| `theRectanglesClosedFormIsTheIntegral` | The rectangle against a brute-force sum at five points and three heights, its far field as a point charge, and a rectangle of no area |
| `theCellsAreGroupedIntoConductors` | Two strips as two nets, every cell belonging to one, and nothing meshed refused |
| `aPlateOverAGroundPlaneIsItsParallelPlateValuePlusFringing` | More than the parallel-plate value, the excess proportional to h/a, falling as the plate widens, and C = C0 in vacuum |
| `aMicrostripsImpedanceIsThatOfTheClosedForm` | Both widths against Hammerstad and Jensen, and the line parameters against the relations themselves |
| `twoLinesCoupleAndTheCouplingFallsWithTheGap` | A symmetric matrix, two equal lines equal, a positive mutual term from a negative Maxwell one, the coupling falling with the gap, and which of the two capacitances `selfCapacitance()` means |
| `whatItCannotSolveIsRefused` | Two metal layers, a layer that is no metal, a layer the stack has not, a mesh past the dense limit, and a stack whose images cannot be enumerated |

## Broken on purpose

`docs/feature_gaps/2026-10-24-rf25-quasi-static/breaks.py` holds thirty, which is what a round of
physics needs: J0's own split; a metal taking room in the stack; a stack with no dielectric and a charge
outside it both accepted; an interface reflecting the wrong way round; a ground plane reflecting as a
dielectric; the recursion not carried down through the slabs; the ground's images dropped; an image's
path one bounce instead of the round trip; the multiple bounces not summed; the geometric series stopped
at its first term; the truncation going uncounted; the free-space term left in the integrand; the
acceleration applied past the end of the range; the rectangle's out-of-plane term, its averaging and one
of its corners; every pair taken at the centre; the images left out of the near terms; the charge placed
at the bottom of the stack; C0 solved on the stack as it is; the mutual matrix keeping the Maxwell sign;
a conductor's capacitance being its own term alone; Z0 by `sqrt(L/C)` instead of the engine's relation; a
length of nothing taken as a line; and the four refusals each removed.

## What the breaks taught

Five of the first twenty-nine went uncaught, and every one named a case the tests had never visited.
Four were test gaps; the fifth was a defect.

- **The recursion was never walked.** Breaking the generalized reflection's descent through the slabs
  changed nothing, because all three stacks in the comparison had *one* slab - the loop body never ran.
  There are four- and five-slab stacks in it now.
- **The charge was never on an interface.** Breaking the subtraction of the free-space term changed
  nothing either, because that term is only non-zero when the charge lies *on* a face of its slab - which
  is the normal case for a metal, and the microstrip's own, and was in no comparison. It is now.
- **Comparing two transforms is a blunt instrument.** Both of those breaks are formulation errors, and the
  test that should have caught them compared the two methods *after* each had been transformed to the
  space domain, where quadrature error muddies the comparison. `theImagesSumBackToTheSpectralReflection`
  compares the two recursions directly as functions of k, with no integral in between, and catches both at
  1e-7. It is the sharper test, and it should have been the first one written.
- **A one-sided assertion is half a test.** `truncated() < 1e-9` passes just as happily when the counter is
  broken and reports nothing at all. There is now a stack built to truncate - two faces that each reflect
  99.8 % - which must report that it did.
- **Two redundant accountings make each other untestable.** Terms fall below the floor in two places - as
  a product is formed, and again once equal paths have been added together, where they can cancel - and
  breaking either alone changes nothing measurable, because the other still counts. The break edits both,
  which is the behaviour rather than one line of it.
- **And that found the defect.** The stack built to truncate reported *nothing* truncated: the geometric
  series' own iteration cap stopped after 2000 bounces and returned silently, so everything past it was
  dropped without being counted. It is counted now, the cap is 2000 rather than 200, and `extract()`
  refuses a stack that loses more than a thousandth of its reflection rather than answering with a
  capacitance that is quietly short of the truth. A stack that reflects that strongly is not physical for
  a chip, but the solver should say so rather than be wrong.
- **With one conductor, two different capacitances are the same number.** `selfCapacitance()` is the charge
  on a line when it alone is at a volt - the Maxwell diagonal - and not the mutual matrix's diagonal,
  which is its capacitance to ground alone. For a single microstrip those coincide, so breaking it changed
  nothing; the coupled pair now holds it to the Maxwell diagonal and to being *larger* than the
  to-ground one.

Fixing the branch threshold that the recursion break exposed made the integral accurate enough that the
images-versus-integral check could be tightened from a part in 1e4 to a part in 1e6. It also made one
earlier fix moot: the guard that stops the series accelerator being applied once the intervals have
covered the whole range is still there and still right, but with the panels now taking the middle
distances no test reaches it, so it carries no break - a guard, not a behaviour.

## What the round taught

- **A branch threshold is a correctness question, not a performance one.** The integral switched from
  graded panels to the between-the-zeros series at `rho k_max = 25`, meaning J0 had turned about four
  times - far too few for an alternating series to be summed from, and the four-slab stack came out 1.8e-3
  wrong because of it. The panels are the accurate method and now run until J0 has turned three hundred
  times, with each panel further split so that none spans more than half a turn; the series takes over
  only where panels really would be thousands.
- **An accelerator applied to a converged sum makes it worse.** The far-field integral sums the pieces
  between J0's zeros and averages the partial sums. When the intervals reach the end of the range the
  plain sum *is* the whole integral, and averaging it with earlier partial sums moves it away from the
  answer: the ground-plane case was out by 4e-5 for exactly that reason. The averaging is now an
  accelerator only, used when the series settles before the range is exhausted.
- **A table was the wrong answer to a slow integral.** The first version tabulated the reflected part over
  log-spaced distance and interpolated; linear interpolation of something curving like
  `1/sqrt(rho^2+z^2)` was good to a part in 1e5, a cubic through `rho` times it to a part in 1e8 - and
  the image expansion made the whole table unnecessary, being exact and closed form. The lesson is the
  order of the questions: it was worth asking whether the quantity had a closed form before building
  machinery to approximate it.
- **A bound pulled from the air is not a test.** The plate test first asserted that fringing adds less
  than the parallel-plate value itself, which a 40 µm plate 10 µm up fails - it adds 1.17 of it, and
  rightly, since a/h = 4 is nearly all edge. Measuring the three plates showed the excess is 4.7, 5.0 and
  5.3 times h/a, which is the edge correction's own form with the logarithm it carries. The test holds to
  *that* now, which is physics, where the first version held to a number I had guessed.

## Not done, and said plainly

- **One metal layer at a time.** The Green's function has no cross-layer term: a charge on M1 and a point
  on M2 needs the transmission through the interfaces between them, which is the next round. A model
  whose cells are on two layers is refused with that sentence, not answered wrongly - so a via farm, an
  inductor with an underpass, and anything with two metals are all out for now. Every benchmark the
  proposal names for the static solver is a single-layer one, which is why this slice is worth having
  on its own.
- **No R, and L only in the TEM sense.** `L = 1/(c0^2 C0)` is exact for a uniform line and says nothing
  about an inductor; the partial inductances the proposal names for that, and R from the surface
  impedance, are not here. The metal's thickness is read and ignored.
- **A dense matrix, capped at 4000 unknowns.** No acceleration, no iterative solve: the cap is a refusal
  with a reason, as the mesh's own is.
- **Nothing calls it.** `extract()` is library code with tests, exactly as `processLayout()` and
  `meshOf()` were before `8f2df86` wired them in. The panel does not show a capacitance, there is no
  Results node behind it, and no tool asks for one. That wiring is the next round, and it should be
  cheap: the Results node and the tab's notes already exist.
- **No lossy substrate, no tan delta.** The dielectrics' losses would give G, and a conductive substrate
  changes the Green's function; both are phase B's own remaining work.
- **The two-length trick is the test's, not the solver's.** Getting a per-metre number from a layout means
  solving two lengths and differencing; `lineParameters()` takes a length and divides. A model that knows
  it holds a uniform line could do the differencing itself, and should.
