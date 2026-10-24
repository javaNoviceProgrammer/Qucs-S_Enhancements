# RF 2.5D, phase B: a charge on one metal seen from another

`2026-10-24-rf25-quasi-static.md` left one sentence at the top of its *Not done*: **one metal layer at a
time.** The Green's function had no cross-layer term, so a model whose cells were on two metals was
refused - which rules out a via farm, an inductor with an underpass, a MIM capacitor and two lines
crossing, that is to say most of what anyone draws. This is that sentence closed.

## What the next phase is

Phase B is the quasi-static extraction: "charge MoM with the static layered Green's function for C,
partial inductances for L, R from the surface impedance - RLGC, Z0, and S-parameters below the first
resonance". Of that, C and a uniform line's Z0 landed in `aea8c4c`. Going on to phase C (full-wave) would
be wrong twice over: the proposal says plainly that phases A and B together are where the work should
stop if it stops anywhere, and that C is the one that slips. So the next phase is the rest of B, and
cross-layer coupling is the first piece of it because everything else - the partial inductances, the
ports, a real structure of any kind - stands on it.

## The recursion, and how it was got right

In the spectral domain the potential of a charge in one slab seen from another is the wave that leaves
the source's slab, crosses each interface between them, and then does what it does inside the point's own
slab:

```
B_s   = [e^-k·leave + R_dn e^-k(2h_s - leave)] / (1 - R_up R_dn e^-2k·h_s)     (leaving the source's slab)
A_j+1 = B_j (1 + r_j)                                                           (through one interface)
B_j+1 = A_j+1 e^-k·h / (1 - R_ahead · r_back · e^-2k·h)                         (across a slab it passes through)
phi   = A_m [e^-k·arrive + R_up,m e^-k(2h_m - arrive)] / (1 - R_up,m · r_back · e^-2k·h_m)
```

The piece that is easy to get wrong, and that I did get wrong first, is **`r_back`**: the reflection a
wave meets looking back the way it came, from inside a slab it is only passing through. The generalized
reflection - everything below, recursively - is the wrong one there, because the paths that dip back down
past that interface are already counted in the transmission that brought the wave up. It has to be the
single interface just crossed. With the generalized one the answer is out by a factor approaching two at
low k and right at high k, which is exactly the signature of double-counted multiple reflections.

That was found, not reasoned out, and the finding is the method worth keeping: **the layered problem
solved outright, as a reference.** A few dozen lines solve for the coefficients of `A e^kz + B e^-kz` in
every slab with the interface conditions and the charge's own jump, by a linear solve rather than a walk
up the stack - a genuinely different method, not a rearrangement of the same one. It is in the tests as
`byLinearSolve`, and `theRecursionAgreesWithTheProblemSolvedOutright` holds the module to it over ten
stacks of up to five slabs, both directions, grounded and open, charges on boundaries and inside slabs,
across five decades of k: **agreement to 1e-7 of each case's own scale.**

Writing that reference first is what made the rest quick. It also caught two other things on its own: the
reciprocity factor (a layered Green's function is symmetric with no permittivity ratio between the two
heights) and, later, that two metals with one dielectric between them are in the *same slab at different
heights* - the MIM capacitor - a case that had lost its direct term entirely.

## Keeping it affordable

Enumerating every path is combinatorial, and it showed: five slabs ran to eighteen thousand images and
twenty-seven seconds. Three things fixed that without giving up the closed form.

- **A term is judged by what it is worth, not by how big it is.** An image at distance `z` contributes at
  most `a/z`, so the floor is on `|a|·z_ref/max(z, z_ref)` against the stack's finest length. An image a
  hundred slabs away must be a hundred times stronger to be kept.
- **What is pruned is not thrown away.** Its charge and its first moment are kept and put back at the end
  as one image at their own mean distance. That is right in both limits - near the charge those paths
  were worth nothing, far from it only the total charge is felt - and wrong between them by no more than
  the terms it replaced. So `truncated()` (what nothing stands for) and `coarsened()` (what the lump
  stands for) are now two different numbers, and the solver reports both.
- **The expansion is built when something first asks for it.** A `Green` asked only for its spectral
  value - which is what the reference test does - pays nothing for an expansion it never reads. That one
  change took the Green's function suite from **18.4 s to 87 ms**.

And for the solver, a table over distance, filled from the images once and read with a cubic: a stack of
three slabs runs to six thousand images, which is cheap to build and far too slow to sum for every pair of
cells a method of moments has. The near terms take the strongest sixty-four over the rectangle in closed
form and the rest from the table, since an image that far away hardly changes across one cell.

## The extraction over several metals

- A Green's function for each **unordered** pair of metals, built once and shared. Unordered because it
  is symmetric, and building it both ways round would give two tables whose interpolation differs a
  little - which would make the capacitance matrix asymmetric for no physical reason.
- **A via joins what it lands on.** Conductors on the two metals a via names, where it overlaps them in x
  and y, become one net - by union-find, so a chain of vias joins a chain of metals. A via carries no
  charge of its own: in the thin-metal approximation it is a wire between two sheets, and its sidewall's
  charge is small beside theirs. So its cells are not unknowns, only the joins they make.
- Each metal is placed where it sits once the metals are collapsed to sheets: the top of the dielectrics
  below it.

## What it agrees with

| Case | This | The reference | Apart by |
|---|---|---|---|
| The recursion, 10 stacks to 5 slabs, both ways, 5 decades of k | the walk up the stack | the layered problem solved outright | < 1e-7 of each case's scale |
| A charge seen across one interface | images | the `2/(e1+e2)` transmitted image | < 1e-8 |
| The same with equal permittivities | images | Coulomb's law | < 1e-8 |
| The charge and the point changed places | images | each other | 6e-12 near, < 1e-5 far |
| Two plates facing each other, a/d = 10, 20, 40 | 1.5, 1.3, 1.2 × the parallel-plate value | the fringing falling as they widen | monotone |
| Joining two nets with a via | one net's charge at a volt | every term of the two-net Maxwell matrix | < 1e-9 |
| Microstrip, W/h = 1 and 3 | Z0 50.07 and 25.86 Ohm | Hammerstad and Jensen | 1.7 %, 0.3 % - unchanged by all of this |

That last row is the one I watched: the single-layer answers are bit-for-bit what they were before the
Green's function was rebuilt around two heights.

## Tested

The eleven tests of the quasi-static round still pass unchanged. New:

| Test | What it holds to |
|---|---|
| `theRecursionAgreesWithTheProblemSolvedOutright` | The whole kernel - reflections and transmissions, one height and two - against the layered problem solved by a linear system, over ten stacks and eight wavenumbers each |
| `aChargeSeenAcrossOneInterfaceIsItsTransmittedImage` | The closed form one interface has, no free-space term of its own across two heights, and Coulomb's law again when both sides have one permittivity |
| `theChargeAndThePointMayChangePlaces` | Reciprocity over three stacks and four distances, which holds through every reflection and transmission |
| `whatTheTruncationDroppedIsCounted` | A stack built to exhaust its bounces reporting what nothing stands for, apart from what the lump does stand for |
| `twoPlatesFacingEachOtherAreAParallelPlateCapacitor` | The parallel-plate value and the fringing above it, falling as the plates widen; a symmetric matrix; most of each plate's charge on the other |
| `aViaJoinsTwoMetalsIntoOneNet` | Two conductors becoming one net named `M1+M2`, the via adding no unknowns, the joined charge being every term of the matrix the two had apart, and a via that lands on nothing joining nothing |
| `aPlateOverSeveralDielectricsIsTheirSeriesCapacitance` | A plate over four dielectrics against `eps_0 A / sum(d_i/eps_i)`, with the edge correction proportional to the stack's whole height and falling as the plate widens |
| `theTableAndTheSplitAreTheImagesAgain` | The table against the images it was filled from over four stacks and six distances, and the near/far split adding back up to all of it for one, eight, sixty-four and every term |
| `linesOnTwoMetalsCoupleWhereTheyCross` | A positive mutual term between lines on two metals, growing as the layers are brought together |

## Broken on purpose

`docs/feature_gaps/2026-10-24-rf25-cross-layer/breaks.py` holds seventeen: the interface transmitting what it
reflects; a slab the wave passes through reflecting everything below it rather than the one face crossed
(the bug this round began with); the point and the charge measured from the wrong faces; the wave that
went the other way first; what comes back off the far face; the direct way between two metals in one
slab; a free-space term given across two heights; a term judged with no regard to distance; the
coarsening counted as missing; the table read straight; a via joining nothing, joining
everything, or carrying a charge; every metal placed at the bottom; the far images left out of a near pair;
and a cell given the
Green's function of its own layer.

## What the breaks taught

Nine of the first twenty went uncaught. Three were only over-broad lists - a break of the downward path
cannot be seen by a test that solves upward - and six were real gaps, each naming something the tests
could not see:

- **The table was never held to the images it is filled from.** Reading it with a straight line instead of
  a cubic changes a capacitance by a part in a hundred thousand, which no benchmark at a few per cent will
  ever notice. `theTableAndTheSplitAreTheImagesAgain` holds the two together directly, and while it was
  being written it showed the cubic was good to 3e-5 at the knee where the nearest image sits - so the
  table has twice the samples now and is good to 1e-5.
- **Nor was the split the near terms rest on.** The strongest sixty-four images are integrated over a
  cell and the rest taken whole; that the two add back up to all of it is an exact identity, and it is
  held for one, eight, sixty-four and more terms than there are.
- **The MIM capacitor is two heights in the *same* slab**, not two slabs, so the break that removed the
  direct way between them touched a branch no test reached. The sum-back test has that case now, and two
  more.
- **The far images matter enormously, and only a deep stack shows it.** Dropping everything past the
  strongest sixty-four images from a near coefficient changes a microstrip by a part in 1e11 and a
  four-slab stack by *four per cent* - the shallow benchmarks simply cannot see it. There is a plate over
  four dielectrics now, held to the series capacitance `eps_0 A / sum(d_i/eps_i)` with the same edge
  correction as over one: measured at 2.20, 2.10 and 1.94 times h/a as the plate widens, where without
  the far images it reads 2.27, 2.47, 4.81 - rising where it should fall.
- **Three of the nine could not be caught at all, and that is the finding.** A Green's function per ordered
  pair of layers rather than per unordered pair, and a near-or-far test judged by one cell of a pair
  rather than the wider of the two, were both changes I made to remove an asymmetry in the capacitance
  matrix. Measured: the asymmetry is 3e-5 with them and 3e-5 without. It is collocation's own - a
  coefficient is the charge over one cell seen at another's centre, equal both ways round only for cells
  of a size - and it swamps either. Both changes stay, the unordered pairing because it halves the
  Green's functions built and the symmetric near test because a pair ought to be near or far whichever
  way it is asked, but neither carries a break, and this says why rather than leaving two uncatchable
  ones in the file. The third is the lump that what-was-pruned is put back as: no test sees it either,
  because at the depths the tests reach the pruning is a part in 1e9. It stays, because it keeps the
  total charge right where the pruning does bite - five slabs and beyond - but that is a depth this
  round does not claim and cannot test in reasonable time, so it carries no break.

## Not done, and said plainly

- **Still no R, and L only in the TEM sense.** The partial inductances the proposal names, and R from the
  surface impedance, are the next piece of phase B. The metal's thickness is read and ignored.
- **A dense matrix, capped at 4000 unknowns**, and a via's own sidewall charge is not modelled.
- **Nothing calls it.** `extract()` is still library code with tests: the panel shows no capacitance,
  there is no Results node behind it and no tool asks for one. That wiring is now the largest single thing
  standing between this and a user, and it is cheap - the Results node and the tab's notes already exist.
- **Deep stacks are coarsened, not exact.** Five slabs across still runs to the term cap; the answer is
  honest about it (`coarsened()` says how much), and the real fix is the discrete-image *fit* the proposal
  names - a handful of complex exponentials fitted to the spectral function, whose cost does not grow with
  the number of layers - rather than enumerating paths. That is a round of its own, and it is what a full
  BEOL stack would need.
- **Collocation, not Galerkin.** The capacitance matrix is symmetric to about 1e-4 rather than exactly:
  a coefficient is the charge spread over one cell seen at another's centre, which is only equal both ways
  round for cells of a size. A second integral per pair would make it exact and cost the fill time.
