# RF 2.5D: a thin film of high permittivity, its Green's function bounded, and a solve that stops

The reviewer's file of 3 November, `~/Desktop/qucs-reviews/RF2p5D/2026-11-03-claude-rf25-thin-layer-green-blowup.md`:
a quasi-static solve of a plain coplanar line - 1 mm, a 50 µm signal, 6 µm gaps, 150 µm grounds - on the thin-film
lithium niobate stack of Nelan et al. (Opt. Lett. 2022) never finished. It ran for over 40 minutes at 160-210 % CPU,
with the process at 8.6-12.9 GB and growing, and was killed; nothing stopped it. The stack, bottom up: nothing under it,
Si 500 µm (εr 11.7, tan δ 0.01), SiO2 4.7 µm (3.9), LN 0.3 µm (28), SiO2 0.5 µm (3.9), Au 1 µm solved as its two faces.

The cause was the reviewer's: the image expansion of the stack's Green's function, which a film reflecting 0.755 at
each face makes combinatorial. Reproducing it found three things more: the spectral integral the images are held to
was itself wrong far from the charge on such a stack, the tab's Stop and a second `rf25_solve` each left a quasi-static
solve running, and the line's capacitance on the reviewer's own mesh is 10 % short, which the review's requested test
would have met at once.

| Feedback | Now |
|---|---|
| **The cause**: every series runs into the 20 000-term cap; `times()` forms every product of two before it prunes (4e8 pairs, 6.4 GB for one list) | **Bounded.** Path lengths are gathered into bins - 64 to an octave - as the products are formed, so no list ever holds more than its bins. A bin keeps two lengths apart, and a crowded one hands each term to the four nodes about it by the cubic through them: linear in the coefficients, so that paths of one length and opposite sign still cancel. The thin-film stack's series is 712 images in 6.6 million pairs and 40 ms (it was 20 001 images, 65 s and 4.7 GB for one build in a driver), and the same function as the spectral integral to a part in 10^8 of the reflected potential from 0.1 µm to 3 cm |
| 1. Bound `times()` as it goes | Done, as above: products go straight into their bins |
| 2. Judge worth against a lateral scale, not the thinnest slab | Not needed: the count is the bins', whatever the floor, and the floor went *down* (1e-13 to 1e-16) - the lump the old one coarsened into one image left the potential 3e-6 out at a centimetre, 1e-3 with the cap |
| 3. Fall back to the spectral integral when a series will not converge | Judged not needed: every series converges now in bounded size - squaring sums a slow series in as many products as its bounces have binary digits - and each stack tried builds in under two seconds (below). A bound remains, with a clear error (5). The integral was fixed instead: it is the reference the images are held to, and it was wrong there (two faults, below) |
| 4. Reuse across the G nudges | The builds cost milliseconds now (four nudged builds of the thin film, 0.2 s). What remained was the solve: each lossy layer filled and factored the whole matrix again, read by nothing for a uniform line, whose loss is its cross-section's. A uniform line is now solved twice (with the stack, in vacuum), its lossy layers on the cross-section alone: the thin-film line 15.2 s to 4.3 s on its gap-graded mesh |
| 5a. A term or time budget with a clear error | **5 billion pairs**, then: "The stack's images did not settle in 5000 million products of their terms: its layer LN, 0.3 µm of εr 28, reflects 0.755 at its lower face and 0.755 at its upper, the most of any. The bound is on the stack's Green's function, which the mesh does not change - fewer layers, or that one's permittivity nearer its neighbours', settle sooner." (A mirror of forty films needs 2.6e8.) |
| 5b. Cancel in the tab, and a stop tool for `rf25_solve` | **The Stop stops it.** The tab had a Stop (the Solve button's and the bar's), but for a quasi-static solve it let the job go and the thread ran on to its end. The extraction now looks at the stop between its steps, at every row of a matrix it fills, and at every product of a Green's function's expansion: stopped, it ends within milliseconds (1 ms in a Green's function, 11 ms in a matrix, 4 ms from the tab). **`rf25_stop`**: as the tab's Stop, waiting up to 'timeout' (30 s) for the thread to end; nothing solving, it says so |
| 5c. A progress line ("building the stack's Green's function: n images") | The bar's tip, the status line, the tab's account and the Results node say the step: "Solving quasi-statically: building the stack's Green's functions, 1 of 2 (the last of 712 images)…", "solving the charge on the cells", "…again in vacuum, for C0", "solving the line's middle cross-section", "solving again for each lossy dielectric's share of the field, 2 of 4" - counted on the bar where it counts |
| 5d. `rf25_solve` refusing or joining a second solve | **Joined, or refused.** A solve going on (the tab's, or an earlier call's past its timeout) asked alike - the same solver, the study's when none is named, a full-wave one's frequencies and matrix - is waited for, not begun again; asked otherwise, refused with what is solving. The second call had let the first go - which ran on, unstoppable - and begun another beside it: the reviewer's doubled CPU. `rf25_inductance` joins a quasi-static solve too |
| 6. The timeout message ("mesh it more coarsely...") | Says where the time is, and leaves the solve going: "It is still solving after 300 s: Solving quasi-statically: building the stack's Green's functions, 1 of 2… It goes on - rf25_solve asked again waits for it, and rf25_stop stops it. The time is in the stack's Green's functions, which the mesh does not change: fewer layers, or thin ones of permittivities nearer their neighbours', build sooner." In the charge solve it says a coarser mesh; full-wave, a coarser mesh or fewer frequencies. Stopped at its timeout, a solve longer than one call could never finish |
| Smaller: the status bar's "924 cell(s), 924 to solve the charge on, 6.5 MB" against the log's 1848 | The bar counts the charges the solve has - a metal solved as its two faces has each cell twice: "924 cell(s), 1848 to solve the charge on, a dense matrix of 26.1 MB" (`Mesh::charges`) |
| Smaller: an RF 2.5D solve that cannot be stopped takes the whole app, and Claude's tools, with it | It stops (5b), and `rf25_solve` no longer waits forever on one: past its timeout it answers |
| **Test to add**: the 1 mm CPW on the stack, finishing in seconds within a fixed memory budget, its C within ~1 % of a 2D cross-section | `aCoplanarLineOnThinFilmLithiumNiobate`: 4.4 s; the Green's function 712 images (the bound: pairs, a count, not a time); C 163.34 pF/m against 165.22 of a spectral-domain solve of the cross-section (-1.1 %), C0 39.95 against 40.505 of its conformal mapping (-1.4 %), εeff 4.0888 against 4.0791 (+0.24 %) - on a mesh graded at the line's gaps, below. On the reviewer's mesh as it was meshed, C was 10 % short of both |

## Reproduced

- **The replica** (`tfln_slowwave/bug/green_series_replica.py`, run as is): the reviewer's table to the digit - the
  film as modelled "stopped: times() of 1629 x 20000 terms = 3.26e+07 products" after 141 calls, with a chuck
  7420 x 4362; the four slabs with the LN at 3.9 a plain 905 images.
- **The C++**, one build of the film's Green's function in a driver: 65 s, 4.7 GB at its peak, 20 001 images (the
  cap), 5e-5 of the reflection coarsened into one image. In the app the metal solved as its two faces adds a slab of
  air and three pairs of heights, each built five times (the stack, and nudged for each of its four lossy layers):
  fifteen such builds, which is the forty minutes.
- **What the cap and the lump left**, against an independent integral (below): 2.4e-5 of the reflected potential
  out at 10 µm, 2.9e-4 at a millimetre, 1.9e-3 at a centimetre.

## The image series

`Green::build()` expands each reflection of the stack symbolically: a series of images `a exp(-k z)`, built by
`up()`, `down()`, `times()` and `overOneMinus()`. A film of 0.3 µm between two of the oxide reflects 0.755 at each
face; the oxide over it 0.592 at the air and -0.755 at the film; the box oxide -0.755 and -0.5; the silicon 0.5 and
0.843 at its foot. Every path through them is a sum of their round trips - 0.6, 1.0, 9.4 and 1000 µm - and it is
worth keeping until a millimetre is thirty round trips of the silicon: a hundred and seventy-five thousand lengths,
filling every series to its cap.

**Bins** (`Gathered`, `kBinBits`). A length's bin is read off its double - its exponent and the top six bits of its
mantissa - so that a bin is a part in 64 to 128 of its length, every series has the same bins, and no logarithm is
taken. A bin keeps two lengths apart (`kKeptPerBin`); a third makes it crowded, and each of its terms then goes to the
four nodes about it - the bins' lower edges - by Lagrange's cubic through them: its charge and first three moments
kept, so that it is good to (1/64)^4 against the term's own potential at any distance. A series of a few paths keeps
them as they are; only crowded bins are changed. `times()` adds each product to its bin as it is formed.

**Why the weights must be linear.** The first attempt gathered each bin's terms of each sign at their own centre,
two points keeping four moments a sign - exact where a bin held two terms - and the film's potential came out
-8.6e304. The series 1/(1 - x) of the film's cavity has `x` of 1628 terms summing in absolute value to 2.48; its
powers fall (2.48, 2.12, 1.69, 1.30 ...) only because paths of one length and opposite sign cancel. Gathered by
sign, `x` summed to 10.3 and its powers grew to 1e306. Weights that are linear in the coefficients - the cubic's -
keep the cancellation.

**Squaring.** `overOneMinus()` sums 1/(1 - x) as (1 + x)(1 + x^2)(1 + x^4)...: a slow series is as many products as
its bounces have binary digits: the film's whole expansion took 57 products where adding powers one by one took
240. The cavity of the truncation test - faces of εr 10 000, keeping all but a part in ten thousand a bounce - ran
out of two thousand bounces and counted what it left out (the test held it to a part in a thousand of the integral);
it converges now, well within the forty squarings (2^40 bounces) allowed, to a part in a million.

**The floor** went from 1e-13 to 1e-16 of a term's worth: what falls below it goes into one image at the mean
distance, and at 1e-13 the film lumped enough a few millimetres down to leave its potential 3e-6 out at a centimetre.

The film's series against the spectral integral (the reflected potential; the direct one is ten to a thousand times
it near the charge):

| ρ | 0.1 µm | 1 µm | 10 µm | 100 µm | 1 mm | 1 cm | 3 cm |
|---|---|---|---|---|---|---|---|
| before (cap and lump) | 4.6e-8 | 1.6e-5 | 2.4e-5 | | 2.9e-4 | 9.7e-4 | |
| now | 5e-10 | 8e-9 | 9e-9 | 2e-9 | 4e-10 | 5e-10 | 3e-9 |

(Before against the numpy integral below, the C++ one being wrong there too; now against the C++ one, fixed, which
meets the numpy one to 1e-10.) Its image charge sums to 2.8999999834 against εr - 1 = 2.9 of a charge in the top slab
of a stack with air on both sides (the far field of a charge in air); on a chuck to -0.9999999976 against -1. Stacks
tried, all against the integral within 6e-8 of what they reflect from 0.1 µm to 3 cm:

| Stack | Images | Pairs | Time |
|---|---|---|---|
| the film (Si / 4.7 / 0.3 of 28 / 0.5) | 712 | 6.6e6 | 0.04 s |
| the same on a chuck | 794 | 9.2e6 | 0.05 s |
| ten layers of incommensurate thicknesses (0.17-3.1 µm, εr 2.1-28) on Si | 885 | 3.9e7 | 0.21 s |
| the same on a ground | 984 | 5.0e7 | 0.27 s |
| a mirror of forty films, 0.1 µm of 28 and 0.2 µm of 3.9, on Si | 901 | 2.6e8 | 1.40 s |

## The integral

`reflectedByIntegral()` - the spectral integral, which rests on no expansion and which the images are held to - was
wrong far from the charge whenever a thin slab makes the range of k long (kMax = 40 over the thinnest length):

- **One panel an interval.** Past ρ kMax = 2000 it is taken between J0's zeros, a ten-point panel each, and the first
  spans every k below 2.4/ρ - where a thick slab's reflections turn on the scale of its own thickness. The four slabs
  with the LN at 3.9 are the two-slab stack exactly, and the integral of the one was 4 % from the other's at 30 µm to
  200 µm. Each interval is now cut where the near branch grades its panels.
- **The plain sum where the intervals run out.** Four thousand of them a centimetre from the charge reach k of
  1.3e6, where the film's reflections are still a tenth; the averaged sum had not settled to 1e-13, and the plain sum
  was taken - an alternating series cut off mid-swing, half its last term out: 1e-3. The averaged sum is taken now.

Both against an independent integral (numpy and scipy, `sommerfeld.py` beside this file: the reflection by its own
recursion, Gauss-Legendre of twenty points on the union of a graded grid and J0's zeros, to where the reflection is
1e-26 of itself): the film at 1 cm 6.1619490776e11 against 6.1619490784e11; the two-slab stack at 10 cm 6.6753611390e10
against 6.6753611292e10.

## The extraction and the tab

`ExtractControl` (`rf25_static.h`) is what an extraction looks at and tells as it goes - its stop, its step
(`Greens`, `Charge`, `Vacuum`, `CrossSection`, `Losses`) with how far into it, and the images of the Green's function
last built. The Green's functions are built up front, each in turn, rather than when the matrix first asked for one,
so that the step is said and a stack that will not settle says so before a cell is solved. Stopped, it returns
"Stopped." at once.

- `RF25Doc` hands its quasi-static job the control; the bar's timer refreshes the tab's account and the panel for a
  quasi-static solve too (it did for a full-wave one alone, which was the only one with anything to count).
- `solvesAtWork()` counts the solves still on their threads, let-go ones included; `waitForSolvesToEnd()`;
  `solvesBegun()`; `solvingAlike()`; `solvingSaid()`; `quasiStaticStep()`.
- `rf25_solve` and `rf25_inductance` past their timeout leave the solve going; `waitsFor()` knows their 300 s and
  `rf25_stop`'s 30.

## Item 4: the losses of a uniform line

G is ω Σ tan δ_k ε_k dC/dε_k, and dC/dε_k was taken by solving the whole structure again with layer k nudged a part
in a thousand: a fill and a factorization of the dense matrix for each lossy layer, beside the stack's and the
vacuum's. For a uniform line nothing read it: its G comes from its middle cross-section (`lineParameters`,
`signalsOf`, `asCoupledLines`, `lossTangent`), which is nudged on its own. A uniform line now solves the whole twice;
its whole loss matrix is its cross-section's over its length (said in `Extraction::dielectricLoss`), and
`Extraction::solves` counts the solves. A structure that is no line is solved whole again for each lossy layer, as
before (`theLossMatrixCouplesAsWellAsLoads`: a pair of unlike lengths, 3 solves). The thin-film line: 15.2 s to 4.3 s.

Each lossy layer is still one Green's function more: milliseconds for the stacks above, but 1.5 s each for the mirror
of forty films, a minute for all forty of them lossy. An analytic dG/dε would carry a derivative of every term
through the same products - the cost of a build for each layer again - so it is not done.

## The line's capacitance, and its gaps

Making the review's test found the line on the reviewer's own mesh 10 % short. Two references of its own, both of a
coplanar line of no thickness - the sheet it is solved as - with grounds of finite width:

- **in vacuum**, its conformal mapping, exact: k = (a/b) √((1 - b²/c²)/(1 - a²/c²)), a = 25, b = 31, c = 181 µm,
  C0 = 4 ε0 K(k)/K(k') = 40.5047 pF/m;
- **on the stack**, a spectral-domain solve of its cross-section (`cpw_sda.py` beside this file): Galerkin, Chebyshev
  functions with each edge's 1/√ singularity on each strip, even symmetry, the charge held to none in all; the
  homogeneous part of the kernel in closed form in space, what the stack adds integrated in k with its reflection by
  its own recursion. In vacuum it gives 40.504687 against the mapping's 40.504721 (8e-7); the four slabs with the LN
  at 3.9 and the two-slab stack give the same 149.940 pF/m; the film C = 165.221 pF/m (165.215 at eight functions a
  strip), εeff 4.0791.

The cells across a conductor were counted on its width alone, and the edges' grading halves the outermost cells once:
a 6 µm gap between a 50 µm signal and a 150 µm ground had one cell of 3 µm on one side of it and one of 9 on the other,
where the charge crowds on both. The static solve against the references:

| Mesh | cells | C pF/m | C0 pF/m | εeff | time |
|---|---|---|---|---|---|
| 4 across, as it was (the reviewer's) | 924 | 149.03 (-9.8 %) | 35.39 (-12.6 %) | 4.211 (+3.2 %) | 5.0 s |
| 8 across | 1260 | 154.19 | 36.93 | 4.175 | 9.4 s |
| 16 across | 2988 | 160.04 | 38.65 | 4.141 | 44 s |
| 24 across | 5356 | 161.53 (-2.2 %) | 39.25 (-3.1 %) | 4.115 | 131 s |
| 4 across, gaps graded to half the gap | 1344 | 161.9 (-2.0 %) | 39.45 (-2.6 %) | 4.104 | |
| **4 across, gaps graded to a quarter** | 1512 | **163.34 (-1.1 %)** | **39.95 (-1.4 %)** | **4.0888 (+0.24 %)** | 4.3 s |
| 8 across, gaps graded to a quarter | 1680 | 163.13 | 39.85 | 4.0937 | |

(The times before item 4 was done were 15 s for the graded mesh.) **`MeshSettings::gaps`**: a side that faces another
conductor of its layer across a gap narrower than its outermost cells has them halved until they are no wider than a
quarter of the gap (`kGapShare`); the gaps found once, by a sweep in x within reach of the grading, and said in the
mesh's lines. The reviewer's model, as it is (its metal two faces, its mesh four across): C0 42.80 pF/m - over the
sheet's 40.50, as 1 µm of metal in 6 µm gaps makes it; it was 36.82, under the sheet's - C 166.34, εeff 3.886.

**The tab sets it for a quasi-static study only.** Every rf25 suite passes with it on for both solvers but one:
`test_rf25_measured`'s coplanar line on alumina (Gold et al. 2019), solved full-wave and held under its measured loss
(its metal smooth where the measured one was rough), came out over it at 30 and 60 GHz (+5.2 %, +4.7 %) - from 18 %
under it on the edges' one halving. Its εeff came 0.4 % from the measured where it was 1.6 %, and its loss at 10 GHz
within 3 % of the incremental-inductance rule's where it was 16 % under. The full-wave solve's loss at the conductors'
edges (the sheet's edge-cell runs, the faces' edge excess) was fitted on the coarser cells; fitted again on these it
may well be better than it was, but that is a study of its own, and full-wave meshes are as they were until it is made.
The measured line's solve also took four times as long graded (158 s, from 36).

## Tests

| Test | What |
|---|---|
| test_rf25_green `aThinFilmOfHighPermittivityIsBuiltInBoundedWork` | the film: built under a bound of 2e8 pairs (it forms 6.6e6), its images few hundred; its charge εr - 1; the recursion to 1e-7; the integral to 1e-6 from 0.1 µm to 3 cm; on a chuck, its charge -1 |
| test_rf25_green `aChargeSeenAcrossAThinFilmIsBuiltToo` | across the film: bounded, the integral's, the same both ways round |
| test_rf25_green `aStackThatDoesNotSettleSaysWhyAndAStopStops` | under a tiny bound it says it did not settle, names the film with its faces' reflections, and that the mesh does not change it; a stopped one says so |
| test_rf25_green `theIntegralIsGradedAndItsSeriesSummed` | four slabs with the LN at 3.9 against the two-slab stack they are, 30 µm to 1 mm; far out (10, 30 cm) a stack of films is (εr - 1)/(4π ε ρ) |
| test_rf25_green `aCavityThatReflectsNearlyEverythingConverges` (was `whatTheTruncationDroppedIsCounted`) | the εr 10 000 cavity: nothing left out, and the integral's to 1e-6 (it was within 1e-3, what the truncation threw away) |
| test_rf25_static `aCoplanarLineOnThinFilmLithiumNiobate` | the review's test: C, C0 and εeff against the spectral solve and the conformal mapping (2 %, 2 %, 0.5 %); every step seen; the images bounded; two solves; the whole loss its cross-section's over its length (a quarter of its length under the sanitizers: its values are its middle's) |
| test_rf25_static `anExtractionStopsWhenItIsStopped` | stopped in its Green's functions and in its matrix, the mirror of forty films: it ends within 500 ms, "Stopped." |
| test_rf25_mesh `aSideFacingANarrowGapIsGradedToAQuarterOfIt` | not asked, as before; asked, the four gap-facing sides' cells no wider than 1.5 µm, the outer ones as before, a gap as wide as the cells as before, none without edges; the charges counted, twice on a metal's two faces |
| test_rf25_sparams `theLossMatrixCouplesAsWellAsLoads` | a uniform line's whole loss its cross-section's over its length, two solves; its C's cross-section near the whole over the length but not it (where its loss's was); a structure that is no line solved whole again, three solves |
| test_rf25_doc `aQuasiStaticSolveSaysWhatItDoesAndStops` | the tab: the charges in its status line; its quasi-static mesh graded at gaps and its full-wave one not; the step said in the status line, the tab's account and the bar's tip; a Stop ends the thread (4 ms) |
| test_rf25_doc `aSolveShowsHowFarItHasGot` | the held solve's status line and tip say "starting" (it said "solving") |
| test_qucs_control `anRf25SolveGoingOnIsJoinedAndStopped` | `rf25_solve` past its timeout: what it is doing, that the mesh does not change it, and it goes on; asked otherwise, refused; asked alike, and `rf25_inductance`, joined (one solve begun); `rf25_stop` stops it, and then has nothing to stop; solved whole after |

## The reviewer's model, through the MCP server

`std_qs.qrf25` (its metal two faces, four cells across, 25 µm at most), copied to the scratchpad, solved through
`rf25_solve` of the build-tree app with isolated settings and cache: before the gaps' grading 22.7 s, C 150.46 pF/m,
C0 36.82, εeff 4.087, Z0 44.82 Ω; now, its mesh graded at the gaps, 18.0 s: C 166.34 pF/m, C0 42.80, εeff 3.886,
Z0 39.53 Ω (it never finished at all before this round). Its copy at eight cells across, past a 2 s timeout: "It is
still solving after 2 s: Solving quasi-statically: solving the charge on the cells… It goes on - rf25_solve asked
again waits for it, and rf25_stop stops it. A coarser mesh solves sooner: fewer cells across a conductor, or a larger
largest cell (the Mesh node)."; asked full-wave, "again.qrf25 is solving already, as it was asked otherwise (…)";
asked again alike, joined; `rf25_stop` "Stopped again.qrf25, which was Solving quasi-statically: solving the charge
on the cells…" at once, and a second "Nothing is solving in again.qrf25." Nothing was written to the real cache.

## Timings

Every rf25 suite, Release (this Mac) and build-asan (Debug, address and undefined-behaviour sanitizers, run beside
other work), passed whole:

| Suite | Release | sanitized |
|---|---|---|
| test_rf25_green | 20 passed, 0.3 s | 20, 34 s |
| test_rf25_mesh | 14, 0.5 s | 14, 6 s |
| test_rf25_static | 25, 20 s | 25, 500 s (the line on the film 52 s, a quarter of its length; the stops < 1 s) |
| test_rf25_sparams | 21, 3 s | 21, 48 s |
| test_rf25_inductance | 15, 0.2 s | 15, 30 s |
| test_rf25_wave | 18, 6 s | 18, 121 s |
| test_rf25_fast | 7, 0.2 s | 7, 16 s |
| test_rf25_model, test_rf25_layout | 13 and 11 | 13 and 11 |
| test_rf25_deembed | 19, 9 s | 19, 321 s |
| test_rf25_fullwave | 30, 50 s | in its two parts, as CI runs it: 17 in 303 s, 15 in 417 s |
| test_rf25_doc | 42, 17 s | 42, 75 s |
| test_rf25_measured | 3, 37 s | (skipped there, as always) |
| test_qucs_control | 216, 147 s | its three rf25 functions, 45 s |
| test_mcp_server | 19, 10 s | |

The full ctest, Release: 160 of 160. The e2e scenarios (`scripts/mcp-e2e-scenarios.py`): 97 checks, none failed. The
four lints green. Nothing written to the real cache or workspace. Full-wave suites take as long as before (50 s, 9 s,
37 s): their meshes are as they were.

## Breaks

`breaks.py` beside this file undoes each fix in the source, builds the tests, runs them and puts the source back:
28 breaks, **all caught**.

| Break | Caught by |
|---|---|
| every length kept apart, no bins | the film; across the film |
| a crowded bin to two nodes by a straight line, not four by a cubic | the film; across the film |
| the geometric series a bounce at a time | the cavity |
| the floor at 1e-13 | the film |
| no bound on the expansion; the stop not looked at as it goes; the layer not named | the stack that does not settle (and the extraction's stop) |
| the integral's intervals one panel each; the plain sum where they run out | the integral graded and summed (and the film) |
| the tab's solve not handed its stop | the tab |
| the matrix filled to its end when stopped | the extraction's stop |
| the stack's Green's functions not handed the stop | the extraction's stop (500 ms; the tab's waits 5 s, past the mirror's 1.5 s, and does not) |
| the vacuum's images said as the stack's | the line on the film |
| a uniform line solved whole again for each lossy layer | the line on the film; the loss matrix |
| the step not said; its texts not refreshed; the status line's "; solving" | the tab (and how far a solve has got) |
| the status line counting cells; the mesh's charges not counted | the tab; the gaps |
| no side graded past one halving; gaps graded to a half | the gaps; the line on the film |
| the tab's quasi-static mesh not graded | the tab |
| rf25_solve stopping at its timeout; starting again what it would join; starting what was asked otherwise; blaming the mesh | the tool test |
| rf25_inductance beginning a solve of its own; rf25_stop not stopping | the tool test |

## Left

- **Full-wave meshes graded at gaps.** Its conductors' loss at their edges was fitted on the edges' one halving; on
  graded cells the measured alumina line's loss rose from 18 % under its measurement to 5 % over it at 30 and 60 GHz.
  Fitting that loss again on graded meshes, against `test_rf25_measured`, would let the full wave have them too.
- **A fallback to the integral** (suggestion 3) is not made: no stack tried needs it. If a stack passes the bound,
  it says so and which layer; the integral, now right far out, would be the place to tabulate from.
- **Each lossy layer is a Green's function more.** Milliseconds on usual stacks; 1.5 s each on the forty-film
  mirror. An analytic dG/dε would cost a build per layer too.
- **The reviewer's QFEM cross-section** of the line was to come as the 2D reference; the spectral solve beside this
  file stands in for it (165.22 pF/m, εeff 4.0791, its metal of no thickness). The model as it is - its metal 1 µm,
  solved as its two faces - is a different line: C 166.34 pF/m, C0 42.80, εeff 3.886 here.
- `rf25_inductance`'s partial inductances (`inductanceOf`) look at no stop; they take a second where the charge solve
  takes minutes, and run after it.
