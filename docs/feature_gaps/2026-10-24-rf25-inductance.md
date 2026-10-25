# RF 2.5D, phase B done: an inductor's L — partial inductances, and the current solved for

The last line of the proposal's phase B is *"partial inductances for L"*, and it was the one thing left
in it. This is that, and with it phase B is complete.

## Why this is not the L the earlier rounds already answer

`L = 1/(c0² C0)` is **exact** for a uniform transmission line, and it comes free with the charge solve:
the magnetic problem of a line's cross-section is the electric problem of the same cross-section in
vacuum, turned about. Three rounds have reported it.

An **inductor** is not a uniform line. Its current goes round a loop, the loop's own field threads it,
and no cross-section describes that. For it one needs the current's path and the flux each piece of it
puts through every other — which is Ruehli's partial inductance (1972) and the L of PEEC.

So there are now two quantities called L in this solver, and they are different things:

| | What it is of | Where |
|---|---|---|
| `L = 1/(c0² C0)` | a uniform cross-section, per metre | `rf25_solve`, the line's RLGC |
| the loop's `L` | a current going round, in henries | `rf25_inductance` |

The tools say so to each other explicitly, because a caller who confuses them gets a number that is
wrong by orders of magnitude rather than slightly.

## The method

- **A conductor is cut into straight runs of current** — the pairs of mesh cells that share an edge,
  which is *exactly* what the mesh has been counting as "the unknowns: where current flows" since phase
  A. That count finally pays for itself.
- **Between two parallel runs** the partial inductance is `(mu0/4pi)` times the double line integral of
  `1/r`, which has a closed form in four terms of `s·asinh(s/rho) - sqrt(s² + rho²)`. **Between two
  orthogonal runs it is nothing**, which is what makes an axis-aligned layout cheap: only x with x and y
  with y.
- **A run's own term** uses the geometric mean distance of its cross-section with itself. For a thin
  strip that is `w·e^(-3/2)`, and the formula then reproduces the textbook
  `(mu0 l/2pi)(ln(2l/w) + 1/2)` — not approximately, but as its exact asymptote, which is the anchor the
  tests hold it to.
- **Two collinear pieces of one strip** have no distance between their axes, so they use the same
  geometric mean distance. That is what makes a strip cut into pieces add back up to the whole strip, and
  it is the reason a mesh of a conductor can stand for the conductor at all.
- **The current is not assumed. It is solved for.** Of every current that obeys Kirchhoff's law and
  carries the terminal current, the one that flows is the one of least magnetic energy:

  ```
  minimise  ½ IᵀLp I   subject to  A I = s
  ```

  with `A` the node-branch incidence and `s` the injection. That is a saddle-point system in the currents
  and the node potentials, one node's row dropped since the injections sum to nothing:

  ```
  [ Lp  Aᵀ ] [ I ]   [ 0 ]
  [ A   0  ] [ u ] = [ s ]
  ```

  Its answer is the direct-current distribution — the current crowding to the short way round, which is
  the whole reason to solve rather than to guess.
- **Then** `L = IᵀLp I / I²`, and the same current gives the loop's resistance at direct current,
  `IᵀR I / I²`, which is a different quantity from a line's R per metre and worth having beside it.

## Two things the tests found, and what they changed

Both of these were found by holding the code to a number from outside it, and both were real.

**The path was a half cell short at each end.** A chain of ten cells has only *nine* centre-to-centre
runs, so the solved resistance of a straight bar came out at exactly 9/10 of `rho·l/(w·t)` — the first
and last half-cells carried no current path at all. The current does not stop at a cell centre: it comes
in at the port. Two **stubs** now run from the port to the first cell's centre and from the last cell's
centre to the port, and the bar's resistance is then *exactly* `rho·l/(w·t)`, which the test asserts to
a part in a thousand million rather than to a tolerance.

**A port drives a face, not a point.** With the stubs in, a bar meshed *two* cells across came out a
sixth too resistive, and — worse — **the answer moved as the mesh was refined**, which is the one thing
a solver must not do. The cause is real physics: a current fed at a single point has a spreading
resistance that a current fed across the conductor's end face does not. Which face is not a guess: the
port's point lies on an edge of a processed shape, and the cells with a side on that same edge *are* the
face. Each gets its own stub from one port node, and **the solve itself divides the current among them**.
With that, the resistance is identical at both mesh sizes and L moves 0.2 % between them.

That second finding is the one I would flag to a reviewer: it was invisible at the mesh size I happened
to test first, and it would have shown up to a user as an answer that drifted when they refined.

## Where it shows

- **The Results node** and the tab's notes carry the loop's L, its resistance at direct current and its
  Q; the status line carries the L, since that is the number an inductor's designer came for.
- **Two ports say what the loop is**: the current goes in at the first and out of the second. With one
  port, or none, there is no loop, and it says what is wanted rather than answering.
- **`rf25_inductance`** is a tool of its own — the 136th. It earned that: it is a different quantity by
  a different method with its own caveats, and when it rode along inside `rf25_solve`'s answer the
  description went 391 characters over the cap that `theToolsAreListed` enforces. The cap was right.

## Tested

| Test | What it holds to |
|---|---|
| `aStripsOwnInductanceIsTheTextbooks` | The geometric mean distance being `e^(-3/2)`; the self term **exactly** the double integral at it; and the textbook `(mu0 l/2pi)(ln(2l/w) + 1/2)` plus the `(mu0/2pi)(rho - rho²/4l)` that expanding `asinh` and the root leaves over it — the offset asserted, not tolerated. Twice the strip being more than twice the inductance; a wider strip less; no length or no width giving none |
| `twoFilamentsMutualIsTheirDoubleIntegral` | The mutual exact to a part in a million million at three separations; reciprocal; falling with distance; **antiparallel giving the same size and the other sign**; orthogonal giving exactly nothing; its own term the largest |
| `aStripCutIntoPiecesAddsBackUpToItself` | Two, four and sixteen pieces summing to the whole strip to a part in a thousand million, and the matrix of them symmetric and summing the same |
| `aTwoWireLineIsTheDifferenceOfItsIntegrals` | Out and back exactly `2(self) - 2(mutual)`; per metre within a part in a thousand of `(mu0/pi)ln(d/GMD)`; wider apart more, a wider conductor less |
| `aRectangularLoopIsGroversFormula` | Three rectangles within 2 % of **Grover's own formula** for one; a 1 mm square of 10 µm trace between 2 and 8 nH (the rule of thumb is a nanohenry a millimetre of perimeter); a bigger loop more; and going round the other way the same loop |
| `theRunsOfCurrentAreTheCellsThatShareAnEdge` | A run for each shared edge, each with a length, a width, the metal's own height, thickness and conductivity, and a resistance; a metal that says no conductivity having none |
| `aStraightBarsSolvedInductanceIsItsOwn` | The solved L within 2 % of the bar's own partial inductance; **R exactly `rho l/(w t)`**; R identical and L within 1 % at twice the mesh density; Q being `wL/R`; no frequency giving no Q and not moving L; the largest run carrying the whole amp; and the current going in and out at the two ends it was given, read from the lines it reports |
| `aWidePlateLosesLessThanANarrowStrip` | A wide plate's R and L both below a narrow strip's, over more runs |
| `aNearPairIsAveragedAcrossItsWidths` | A near pair held to a 240x240 midpoint rule over both widths at three separations; the middles alone wrong by per cents inside two widths; and a far pair exactly the middles |
| `aCurrentThatTurnsACornerCrowdsAtIt` | In across one face and out across another meeting it at a corner: a loop, not a refusal; nearly the whole amp taking the short way and the current spreading more than a hundred to one; and both R and L below going the length of the plate |
| `whatHasNoLoopIsRefused` | Four refusals **each in its own words**: no cells; both ends on one face; a layout in pieces nothing joins; a single cell with nowhere for a current to flow; and a mesh of 3736 runs refused by the dense limit before a matrix is set up |
| `theLoopsInductanceIsSolvedFromItsPorts` (doc) | One port saying "wants two" rather than answering; two ports giving a loop of some hundreds of picohenries with a resistance and a Q of `wL/R`; it in the notes, the status line and the Results node; and thrown away by an edit as the capacitance is |
| `anRf25ModelIsSolvedThroughItsTool` | `rf25_inductance` on a 1200 µm wire: L between 0.5 and 10 nH, R above nothing, the runs and cells it took; Q at the study's frequency and at one of its own, each `wL/R` with L unmoved; asking twice leaving no step of Undo; a frequency that is none, a model with one port, two ports in the same place, and a schematic each refused; **and the line's L differing from the loop's by more than half**, which is the two quantities being two |

## Broken on purpose

`docs/feature_gaps/2026-10-24-rf25-inductance/breaks.py` holds fifty-two: the closed form and its four
terms, the permeability, the geometric mean distance, the signs and the orthogonality, the near-field
quadrature, the matrix's symmetry, the runs the mesh gives, the terminal stubs and the face a port
drives, Kirchhoff's law and the energy minimised, and the three places it is said plus the tool.

## What the breaks taught

Seventeen of the first fifty-seven went uncaught, and **a third defect came out of chasing them** - the
best argument for the exercise that these rounds have produced.

**A current may turn a corner.** Hunting for a geometry in which the energy functional could be seen at
all, I fed a plate across its left face and out across its bottom one. It was *refused*: the two faces
meet at the corner cell, which is in both, and the guard compared their cells rather than the faces. A
current going in at one face and out at another that meets it at a right angle is a loop like any other,
and it could not be solved. The guard now compares the faces themselves, and the corner case is a test
in its own right - where, satisfyingly, the current crowds at the inside of the corner by five orders of
magnitude, which is the energy minimisation doing exactly what it is there for.

The other sixteen were the tests, in four kinds:

- **A lossless, symmetric, one-cell-wide fixture hides nearly everything.** The straight bar could not
  see which face the current entered (it is symmetric, so swapping the ends gives the same answer), nor
  whether the energy minimised was the magnetic one (a single path has only one admissible current, so
  the functional never matters). The bar now asserts *where* the current went in and out, by reading the
  lines it reports; the corner-turning plate carries the functional.
- **A comparison taken from the thing it compares against proves nothing.** `aStripCutIntoPieces-`
  `AddsBackUpToItself` takes the whole strip's self term by the same call it is testing, so a break in
  that term is self-consistent and invisible. It keeps its value as a consistency check of the kernel,
  but the break file now names only the test that *can* see each break, rather than listing every test
  that touches the code.
- **The near-field quadrature had no test at all.** The cut-strip pairs are all collinear, and collinear
  pairs take the geometric-mean-distance branch before the near/far test is ever reached - so four
  breaks in the quadrature changed nothing anywhere. `aNearPairIsAveragedAcrossItsWidths` is new: it
  holds a near pair to a 240x240 midpoint rule over both widths, and asserts that taking it between the
  middles instead would be wrong by per cents, which is why the averaging is there.

  Three of those four breaks it catches. The fourth it cannot, and the reason is worth keeping: putting
  both widths the *same* way round - `offset + ga·wa + gb·wb` rather than `- gb·wb` - gives bitwise the
  same answer. The Gauss nodes are symmetric about zero and carry matching weights, so flipping the sign
  of one of them merely permutes the node set and the weighted sum is unchanged, whatever the two widths
  are. The minus is the separation of two points and is the right thing to write; it is simply not a
  behaviour, so it carries no break.
- **A guard backed by the next guard is invisible unless the message is read.** Four refusals - a
  singular system, a mesh with no runs, a mesh over the dense limit, an energy of nothing - each fell
  through to a later check, so removing any one of them still refused and the test still passed. The
  refusals are now asserted *in their own words*, and the dense limit has a mesh fine enough (3736 runs
  over 1920 cells) to actually reach it.

Five breaks were dropped rather than fixed. One is the quadrature's sign above, which changes no number
by symmetry; the other four are guards that cannot be shown to matter: a width of
nothing already gives a distance of nothing (`0 * e^-3/2`), a run of no length is already stopped by the
orthogonality test above it, a frequency of none already gives `Q = 0`, and an energy of nothing is
unreachable from any real extraction. A fifth - the tool waiting for the mesh - is real but cannot be
demonstrated on a layout this small, since opening the model already finishes the read. Each is said
here rather than left in the file as a break nothing can catch.

## Not done

- **The current is the one at direct current.** The energy minimised is magnetic alone, so the answer is
  the low-frequency L. At a frequency the current leaves the inside of the metal and L falls a little
  towards its high-frequency value; that wants `(R + jwL)i = v` solved at each frequency, which is the
  next thing and would also give the loss properly.
  *(Corrected on 25 October: this had it backwards. At direct current the resistance divides the current,
  not the inductance; the current of least magnetic energy is where a current goes once its inductance
  outweighs its resistance. Its loss, given as the resistance at direct current, counted its crowding as
  resistance - 29 % high on a bar ten cells across, which the finer default mesh of the phase B check
  showed. R at direct current, and the inductance of that current beside the other, now come from the
  network's own resistive solve: see `2026-10-25-claude-rf25-phase-b-check.md`.)*
- **The thickness is in the resistance but not the geometry.** The geometric mean distance is the thin
  strip's, consistent with the thin-metal approximation the rest of the solver makes, and good to a few
  per cent while `t/w` stays small — which on a chip it does.
- **No return path through the ground plane.** The loop must be within the meshed conductors. A
  microstrip's return is the ground plane, which is a layer and not a conductor, so a *line's* loop
  inductance cannot be had this way — which is no loss, since `1/(c0² C0)` is exact for it.
- **The loop is the first two ports.** More than two are reported but only the first two used; a mutual
  inductance matrix between several loops is the natural next step and is what a transformer wants.
- **The currents are not drawn.** They are solved for and kept (`Inductance::current`), and the proposal
  has them on the layout after a solve; nothing paints them yet.
- **A via's cross-section is one number.** A run in z has a two-dimensional cross-section and one side of
  it stands for its breadth, which is the same approximation the thin metal makes and is small beside a
  via's own short length.
