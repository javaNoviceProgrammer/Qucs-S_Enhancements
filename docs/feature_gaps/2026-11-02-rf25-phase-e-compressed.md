# RF 2.5D, phase E: the compressed solve

The user's "ok, let's move to the next phase of RF 2.5D" (2 November), after the GSSG round (`0017af4`). The
proposal's phase E is "acceleration: ACA or the adaptive integral method on a grid, GMRES with a near-field
preconditioner, the memory bound lifted from a few thousand unknowns to a few hundred thousand". Its decision 5
left the choice open: "ACA/H-matrix (simpler, kernel-independent) or AIM/pFFT (faster for planar, more work)".
This round takes ACA, for the reason the proposal gave and one more: the solve's entries are not one kernel
between points. They are closed forms between rectangles and segments for the near pairs, the stack's images
and tables at each pair of heights, and the vias' corrections. A compressed matrix that needs nothing but the
entries holds all of that as the dense solve has it, and its answer goes over into the dense solve's as its
tolerance goes to nought.

Past 6000 runs and cells the full-wave solve now holds its system compressed and solves it by iteration. Some
310 000 unknowns, a patch 2 mm square in cells of 6.25 µm, take 25 s and 6 GB at a frequency on this machine,
where the dense matrix would be 1.5 TB.

## The method

`rf25/rf25_fast.{h,cpp}`, new, holds what is not RF:

- **A cluster tree.** The unknowns' boxes are halved again and again at the middle item, along the longest side
  their centres span, down to leaves of 32 or fewer.
- **An H-matrix.** Each pair of clusters is a block:
  - Where the smaller is no wider than 1.5 times their distance, the kernel is smooth across the pair, and the
    block is of low rank.
  - A low-rank block is found by adaptive cross approximation (Bebendorf). It takes a row and a column of the
    block at a time, each where the last left the most out. It stops when two outer products running each
    add less than the tolerance of what is held, and is then recompressed: a QR of each side and an SVD of
    the small core between them.
  - Only those rows and columns of the kernel are ever computed. A rank that would hold as much as the block
    itself is not taken: the block is filled dense.
  - The near pairs are dense, as the matrix has them.
  - A symmetric matrix is held as its upper blocks, each applied both ways round, so it stays exactly
    symmetric.
  - The blocks are filled on as many threads as there are, and multiplied on them where there is work enough.
- **GMRES.** Restarted every 100 vectors, orthogonalized twice by modified Gram-Schmidt, and preconditioned on
  the right, so that the residual it stops on is the system's own. The residual is taken again at each
  restart, not the recurrence's.

`rf25_fullwave.cpp` uses it past the dense limit:

- **The entries are the dense solve's own.** The fill of one frequency became a `Kernels` object: the Green's
  functions of every pair of heights and each via's, and an entry function for each matrix. The dense solve
  calls them with the static parts it made once for every frequency, and the compressed one makes them as it
  goes. Both solve the one matrix, and for an axis-aligned layout the dense solve is bit for bit what it was.
- **L is two kernels.** `L_ab = (x_a x_b + y_a y_b) K_ab`: the cosine between two runs times a kernel smooth in
  where they are. So `L I = X Kx X I + Y Ky Y I`, where X and Y are the runs' directions' parts:
  - `Kx` is of the runs with a part along x, and `Ky` of those with a part along y. An oblique run is in both.
  - Neither kernel is nought between a pair of its runs, as L is between two at right angles. A cross
    approximation samples a block a row at a time, and would take half a block for all of it.
  - An axis-aligned layout's two kernels are half of L: the half between runs at right angles is left out.
- **P, and the vias.** P is one symmetric H-matrix of the cells. Each via's potential at each cell, `Phi`, is a
  rectangular one, and the vias' G_zz a symmetric one. The coupling of a run to a via, `W = -A^T Phi`, is
  applied as `A^T` of `Phi`'s products, both ways round, and half each way between two vias, exactly as the
  dense solve sums it.
- **A is never compressed.** The system is applied as `Zs l/w + jw L + A^T P A / jw + the vias'`, with the
  incidence A exact. A loop of current leaves no charge, and is as free of P in it as in the dense solve,
  however P is approximated. A compressed `A^T P A` would hand a loop the far field's error divided by jw, the
  low frequencies' undoing.
- **The preconditioner.** It is the sparse LU (`fem::SparseLu`: Accelerate's from some 20 000 unknowns, Eigen's
  below) of the same system between the near pairs alone. Those are the runs, cells and vias whose boxes are
  within half of the larger's longest side: those that touch, and the faces of a metal solved as its two. It
  is built with the whole of A, so its loops and charges stand as the system's do: about 30 non-zeros a row.
- **The ports' Z, variationally.** `Z = C - (G^T I + I^T G - I^T M I)`, where G is the columns' drives and C
  their potential of their own charge. This is the dense solve's `C - G^T M^-1 G` where I is `M^-1 G`, and its
  error is the square of I's. M is symmetric, so Z is too.
- **The automatic choice.** A solve is dense up to 6000 runs and cells and compressed past them, or always the
  one asked (`FullWaveControl::matrix`; the study's new **Matrix**, shown for a full-wave study; rf25_solve's
  `matrix`). A dense solve past the limit is refused as before, now naming the compressed one.
- **The memory bound.** Before anything is filled, the trees and the blocks give what a frequency will take:
  the dense blocks exactly, the low-rank ones at a guessed rank of 16, the near field and ten times it for its
  LU, and GMRES's vectors. Past half the machine's memory (`physicalMemory()`), it is refused with the figure.
  Below that, as many frequencies are solved at a time as fit, and the threads are shared out between them. The
  guess was 12.9 GB for the 310 000 unknowns whose peak resident memory was 10.3 GB.
- **The mesh.** Every mesh was cut to 8000 cells, the charge solve's bound, before fewer were put across a
  conductor. For a full-wave study held compressed the bound is 40 000 cells, some 120 000 unknowns, 2 to 3 GB
  and ten seconds at a frequency. The mesh's new **Most cells** overrides it either way. The mesh's cost then
  says the compressed matrix's, some 20 kB an unknown as measured, and the tab's status line says it in place
  of the charge solve's.
- **What it says.** "The system held compressed (N runs and cells, past the 6000 a dense matrix is set up for,
  or as asked) ... to a part in 1e5 of its own size (rank R at most) ... M MB where dense it would be D GB, T
  thread(s) a frequency. Solved by GMRES, preconditioned by the near field's sparse LU: I iteration(s) at most,
  to a residual of r". rf25_solve's answer has `matrix`: held, tolerance, memory MB, dense MB, largest rank,
  iterations, residual.

## What it agrees with

Each structure is solved both ways, compressed at its tolerance of 1e-5 and GMRES to 1e-10, and compared with
the dense solve (`aCompressedSolveIsTheDenseOne`):

| Structure | Unknowns | S, largest difference | Iterations |
|---|---|---|---|
| A 3 mm microstrip on 100 µm of 9.8, 10 MHz to 40 GHz | 368 | 1.2e-7 | 13 |
| 1 mm of 3 µm copper solved as its two faces, its walls vias, 10 MHz and 10 GHz | 428 | 7.7e-10 | 25 |
| A lossy tee, three ports, 12 GHz | 349 | 2e-14 | 15 |
| Lines on two metals joined by a via, 100 MHz and 20 GHz | 198 | 2.6e-7 | 10 |
| A GSSG with no ground plane, coplanar ports in pairs, 2 GHz | 512 | 2.4e-13 | 36 |
| A pad on 144 vias to the ground, 100 MHz | 1276 | 3.6e-9 | 34 |
| A strip at 30 degrees, its runs oblique, 5 GHz | 823 | 8.9e-10 | 16 |
| A coplanar line's via fences, 3 mm, 60 GHz (1.5 wavelengths) | 665 | 1.8e-6 | 27 |
| A 4 mm line ten cells across, 10 GHz | 5658 | 3.3e-8 | 23 |

Z stays symmetric to 2e-13 in every case, and passive as the dense solve is. The last two rows were
measured while the round was built, not kept as tests.

Past the dense limit, lines ten cells across and 4 and 6 mm long (5658 and 8458 unknowns), both compressed:
at 10 GHz the two-line method gives eps_eff 6.5697 against Kirschning and Jansen's 6.5713 from its own
quasi-static 6.5434, and Z0 49.40 Ohm against `sqrt(L'/C')` of 49.64.

How it grows, at one frequency of 10 GHz, lossy copper, on this machine's 24 threads:

| Layout | Unknowns | Time | Matrices | Iterations | Dense |
|---|---|---|---|---|---|
| A line 4 mm, ten across | 5 658 | 0.27 s | 19 MB | 23 | 5.5 s |
| A patch of 2 mm in cells of 25 µm | 20 008 | 2.3 s | 322 MB | 26 | 6.4 GB |
| The same in 12.5 µm | 78 408 | 6.0 s | 1.47 GB | 36 | 98 GB |
| The same in 6.25 µm | 310 408 | 24.8 s | 5.9 GB (peak 10.3) | 48 | 1.5 TB |

The 78 408 took 31 iterations at 10 MHz, 34 at 100 MHz and 48 at 60 GHz.

## What building it found, and what it changed

- **GMRES's residual counts at a low frequency, variational Z or not.** At a residual of 1e-6, the pad on its
  vias was 1e-3 out of the dense solve's S at 100 MHz, and 6e-5 at 1 GHz. A tighter compression did nothing for
  it; a residual of 1e-10 brought it to 2e-10. The variational Z squares the error of the currents, but divides
  that square by what the system is near singular by, and at 100 MHz that is much. A residual of 1e-10 takes
  some 1.7 times the iterations.
- **A wider near field preconditions worse.** The via farms took more iterations than anything else (below),
  and the first thought was to put the vias' couplings to their neighbours into the preconditioner. At a via's
  own width it took 346 iterations where touching alone took 239, and at four widths it did not converge in
  3000. With the vias' whole block in it, both their G_zz and their potentials, it was 197 at six times the
  time. A smooth kernel truncated that far is no longer definite, and a via's G_zz without the potential
  corrections around it that cancel its dielectric part is another inductance. Runs and cells reaching one or
  two of their sizes did the same: worse at one, a little better at two for more time. The near field is the
  pairs that touch.
- **The setup searched every cell for every via.** On a pad with 900 vias, `viaRunsOf()` took 3 s finding the
  metal cell under each via cell, by `containsPoint` on every cell of the mesh. It now looks in bins of the
  plane (`CellBins`). Three more such searches went too:
  - `runsOf()`'s own via search, which the full wave throws away (`runsOf(..., vias = false)`);
  - `netsOf()`'s search of every shape for each cell's conductor;
  - the full wave's search of every shape for each run's free sides.

  The last two now look only at the shapes on the cell's or run's own layer. The farm went from 16.7 to 12.2 s.
- **An oblique run's partial inductance had no cosine.** Between two runs neither parallel nor at right angles,
  `partialInductance()` measured the second along the first's axis from its start, at its full length, and gave
  only the sign of the cosine between them. At 60 degrees it is 2.11 times Neumann's double integral of dl.dl'/r
  along the two. The full wave now takes the cosine times `alignedInductance()`: the partial inductance of the
  two turned about their middles onto the direction between them. That is symmetric, smooth in where they are,
  and Neumann's to 1.8e-4 in the test. Axis-aligned pairs are as they were, to the bit. An oblique run is one
  between cells cut to a slanting outline, whose centres do not line up. Phase B's quasi-static inductance
  keeps the old form (left open, below).
- **One small outer product is not enough to stop on.** ACA stopping at the first outer product under the
  tolerance left the GSSG at 20 GHz 5.4e-7 out of the dense solve's S. Stopping at the second running left it
  2.2e-7, for a few per cent more memory.
- **The tolerance and the admissibility, measured** on the 78 408-unknown patch and the cases above:

  | Tolerance | Eta | Matrices | S, largest difference from dense over the cases |
  |---|---|---|---|
  | 1e-6 | 1.5 | 1.97 GB | 1.2e-7 |
  | **1e-5** | **1.5** | **1.47 GB** | **2.6e-7** |
  | 1e-4 | 1.5 | 0.90 GB | 7.2e-6 |
  | 1e-5 | 2.0 | 1.34 GB | 1.35e-6 |

  Eta 1 held more dense blocks for no better answer (2.2 GB at 1e-6), and leaves of 64 a tenth more memory
  than 32.
- **A block of nought.** A via's potential correction is nothing in a stack of one medium. A cross approximation
  whose first row is nought tries others, eight across the block, before it takes the block for nought.
- **The runs were numbered in a hash's order.** `runsOf()` made its runs by going through a `QHash` of the
  edges two cells share, and Qt seeds a hash anew in every run of the program. The dense solve does not mind
  the order but for its rounding; a compressed solve cuts its clusters from it, and the same line solved in two
  runs of the packaged app came out 1e-8, 6e-5 and 2e-5 dB apart in S11. The runs now come in the order of the
  cells they join (`std::is_sorted` of their ends, in test_rf25_inductance), and two runs of the app give the
  same answer to the digit. Found by the DMG's own drive, after the first commit of this round (`6c93e75`).
- **The strips of a port's feed.** Past the dense limit they were cut to a half and a quarter of their length.
  Unasked, they are now held compressed and solved whole: the test's strips of 1000 µm past a lowered limit give
  the dense solve's line to a part in 1e6. They are cut only for a dense solve asked, or past the memory.

## A limit found: via-shorted pads converge slowly

A pad shorted to the ground through vias takes many more iterations than anything else. The 2 mm pad meshed at
12.5 µm, about 55 000 unknowns, takes:

| Vias | Iterations |
|---|---|
| none | 45 |
| 100 | 151 |
| 225 | 188 |
| 900 | 239 |

It is the vias' shorts, not their dielectric (168 iterations in vacuum) nor their mutual coupling (the whole
via block in the preconditioner: 197). It rises with the frequency (87 at 100 MHz, 222 at 30 GHz) and without
loss (252 with perfect metal). Pinned to the ground at many points, the pad's current is set by its inductance
over the whole pad, which the near field holds only between runs that touch. It still solves, 900 vias in 12 s,
but a preconditioner that holds a structure's global inductance - a coarse level, or an H-LU at a low accuracy
- is the next step for it.

## Tested

| Test | What it holds to |
|---|---|
| test_rf25_fast `aClusterTreeHalvesItsItemsAlongTheirLongestSide` | Every item once; each node its children's halves, its box all of theirs; leaves of the size asked; a row halved along its length; items at one place one leaf |
| test_rf25_fast `aCrossApproximationIsItsBlockToTheTolerance` | Two clusters' retarded kernel to 20 times each tolerance from 1e-3 to 1e-9, its rank growing and below 40; a block of nought, no outer product; a rank that would hold no less, refused |
| test_rf25_fast `anHMatrixMultipliesAsItsDenseMatrixDoes` | A mesh at two heights: the product within 1e-5 of the dense one, its transpose itself; 3136 cells in under 0.35 of the dense memory; four threads the same to rounding; a rectangular one both ways round; stopped, false |
| test_rf25_fast `gmresSolvesToItsResidual` | A complex system to 1e-10; one step with its own LU; restarted every five, the same; nought for nought; stopped; short of iterations, not converged |
| test_rf25_fullwave `aCompressedSolveIsTheDenseOne` | The seven structures above: S within 2e-6 of the dense solve's, Z symmetric to 1e-9, passive as the dense one, fewer than 80 iterations, a slanted strip's runs oblique |
| test_rf25_fullwave `pastTheDenseLimitItIsHeldCompressed` | Past a lowered limit, held compressed unasked and said, its S the dense one's; in Release, lines ten across past the limit: eps_eff within 2e-3 of Kirschning and Jansen, Z0 within 1 % |
| test_rf25_fullwave `whatItCannotSolveIsSaid` | A dense solve past the limit refused, the compressed one named; more memory than a solve may take, refused with the figure |
| test_rf25_inductance `anObliquePairIsTheCosineTimesItsAlignedOne` | Parallel runs as partialInductance's; symmetric; at 60 degrees the cosine times it Neumann's to 5e-3; the old form over 1.8 times it; at right angles a finite kernel |
| test_rf25_mesh `aMeshForACompressedSolveHoldsMoreCells` | Four strips at ten across past the charge solve's 8000 and within 40 000, said compressed; forty strips at three across; Most cells read and obeyed |
| test_rf25_deembed `stripsTooLargeAreCutAndSaid` | Cut for a dense solve asked; held compressed unasked, whole, the dense solve's line to 1e-6 |
| test_rf25_doc `aFullWaveStudysMatrixIsItsOwn` | A full-wave study's mesh said compressed; Matrix shown for it and not for a quasi-static study; a compressed solve for itself, the study untouched, its S the dense one's; the study's own dense and compressed obeyed |
| test_qucs_control `anRf25ModelIsSolvedFullWaveThroughItsTool` | The answer's `matrix`; 'matrix' compressed, what it kept, S the dense one's; a matrix that is none and one of the quasi-static solve refused |

## Broken on purpose

`docs/feature_gaps/2026-11-02-rf25-phase-e-compressed/breaks.py` holds thirty-four breaks. Each is built and the tests named for it run:

- **The cross approximation and the H-matrix:**
  - its row, and its column, without what is held already taken off;
  - done at its first small outer product;
  - recompressed to a hundred times the tolerance;
  - a symmetric matrix's lower blocks left out;
  - every pair of clusters low rank, the touching too.
- **GMRES:** its update not preconditioned; its rotations' sine not conjugated.
- **The system:**
  - L's kernel along y left out;
  - an oblique run in the kernel along x alone;
  - `A^T P A` without the vias' charges;
  - a via's coupling to the runs one way round alone;
  - between two vias their potentials' halves each whole.
- **The ports' Z, and the residual:** Z from the currents as they are, not variationally; GMRES to a residual of
  1e-6.
- **The preconditioner:** none; the near field's P its diagonal alone; the near field without a via's potential
  at the cells beside it.
- **The automatic choice and the memory:**
  - dense past the limit unasked;
  - a dense solve past the limit set up when asked;
  - the memory a frequency would take not judged.
- **The oblique runs:** an oblique pair's aligned inductance its partial one; the dense solve's oblique pairs as
  they were.
- **The mesh:**
  - a compressed solve's mesh held to the charge solve's 8000;
  - its cost said dense;
  - Most cells not read.
- **The tab:**
  - the study's Matrix not handed to the solve;
  - a full-wave study's mesh cut for the charge solve;
  - the Matrix read as automatic whatever it says;
  - the Matrix shown for a quasi-static study too.
- **The tools:**
  - rf25_solve's 'matrix' not read;
  - the answer without what the compressed matrix kept;
  - 'matrix' let through for the quasi-static solve.
- **The runs** in a hash's order, added after the DMG's drive found it.

## What the breaks taught

- **Thirty-two of thirty-four were caught, after one test was tightened.**
  - The first run held every structure to fewer than 80 iterations. The near field's P cut to its diagonal
    passed: its S was right, at about twice the iterations (a line 13 against 30, a via 10 against 18, the slant
    16 against 29).
  - The three plainest structures are now held to a little over what they take (20, 14 and 24), and it is
    caught.
  - Some breaks were caught by other tests than the ones first named for them, and the script now names
    those:
    - ACA's column untaken-off, and the recompression a hundred times too coarse, by the unit tests alone. The
      small structures' full-wave S stayed within 2e-6: their blocks are mostly dense.
    - Every pair low rank, and the rotations' sine, by the full wave alone.
- **Two were left uncaught, and are kept for what was measured of them.**
  - **ACA stopped at its first small outer product.** It is a margin, not a fault. The unit block's error at
    tolerances of 1e-3, 1e-6 and 1e-9 was 1.1e-3, 8.1e-7 and 6.1e-10, where stopping at the second is 0.86e-3,
    6.6e-7 and 6.0e-10. The GSSG at 20 GHz was 5.4e-7 out of the dense solve's S, against 2.2e-7. Both are
    within what any test can hold to without failing on a platform's rounding.
  - **The near field without a via's potential at the cells beside it.** The test's farm of 144 vias converges
    faster without it, 24 iterations against 34. The farms of the measurement converge slower: 181 and 320
    against 151 and 239 for 100 and 900 vias. It is kept for those, and a farm large enough to show it is more
    than a sanitized build solves in a test's time.

Rebuilt after the breaks, the full ctest (150), the scenario script (97 checks) and the three lints (tool
arguments, JSON nesting, third-party names) were green.

## Under the sanitizers

Built Debug with ASan, as CI's Linux job is, and solved with Eigen's sparse LU, as CI's Linux has no
Accelerate:

| Suite or function | Time |
|---|---|
| test_rf25_fast, all | 19 s |
| test_rf25_fullwave `aCompressedSolveIsTheDenseOne` (its sanitized cases: no faces, fewer vias and frequencies) | 84 s |
| test_rf25_fullwave `pastTheDenseLimitItIsHeldCompressed` (the fine lines are Release's) | 4 s |
| test_rf25_fullwave `whatItCannotSolveIsSaid` | 30 s |
| test_rf25_fullwave, all | 456 s (375 last round) |
| test_rf25_mesh, all | 9 s |
| test_rf25_inductance, all | 31 s |
| test_rf25_deembed `stripsTooLargeAreCutAndSaid` | 15 s |
| test_rf25_doc `aFullWaveStudysMatrixIsItsOwn` | 10 s |

CI's runner is about twice this machine there. That puts the full wave near 900 s against its 900 s limit
times the Linux job's scale of three, and its heaviest function near 170 s against QtTest's 900 s.

## Not done

- **A preconditioner that holds the global inductance.** See the via-shorted pads above: 150 to 250 iterations
  where a plain patch takes 45.
- **The quasi-static extraction is still dense.** Its charge solve sets up a dense matrix of up to 8000 cells.
  P is the same kind of matrix, and an H-matrix of the static kernel with conjugate gradients would lift it the
  same way. A quasi-static study's mesh keeps the 8000.
- **Phase B's partial inductance of oblique runs** keeps its old form, without the cosine: the quasi-static
  loop inductance of a slanting layout differs from the full wave's there.
- **A frequency is filled from nothing.** The H-matrix's blocks are found anew at each frequency, and GMRES
  starts each from nought. The next frequency's currents are near the last's, and the near field's pattern is
  the same at every one.
- **The AIM/pFFT alternative** is not built: a planar layout on a grid would hold some 1 kB an unknown where
  the H-matrix holds 20, at the price of the grid's projections and their precorrection.
- **The tab's drawing at these sizes** is the layout view's overlays, one shape a cell. Checked in the built app
  through its tools: a pad of 4224 cells, 12 512 unknowns, solved compressed in some 3 s for three frequencies,
  its mesh and its current density drawn, the status line saying "held compressed, some 238.6 MB" (171 MB
  held). A mesh of 40 000 cells drawn has not been timed.
