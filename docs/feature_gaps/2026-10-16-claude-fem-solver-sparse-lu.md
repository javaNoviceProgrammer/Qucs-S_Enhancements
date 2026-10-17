# The multiphysics solver: Accelerate's sparse LU, and what the review asked after it

The review (`2026-10-16-claude-fem-solver-gpu-vs-sparse-lu.md`, the user's question "is it worth
adding GPU acceleration to the multiphysics (FEM) solver?") measured Eigen's single-threaded
SparseLU factorizing a 681,054-unknown system in 28.5 s, and Apple Accelerate's sparse LU
factorizing the same system in 0.17 s. It recommended, in order:

1. a sparse direct backend behind the existing classes;
2. native complex sources in the waves physics;
3. faster assembly;
4. independent sweep points in parallel inside one Qucs-S;
5. the GPU later.

## 1. A sparse direct backend

| Recommended | Now |
|---|---|
| Accelerate's `SparseFactor` behind `LinearSystem`, `ComplexSystem`, `PdeProblem::factor` and the local `SparseLU`s (waves, semiconductor, quantum); Eigen's as the fallback | `fem_sparse.h`: `SparseLu<double>`, `SparseLu<complex>` and `SparseCholesky`, with Eigen's interface (`analyzePattern`, `factorize`, `info`, `solve`). They replace all 13 SparseLU sites and the linear system's simplicial LDLT. Accelerate's LU, with threshold partial pivoting, where the SDK has it and macOS is 15.5 or later (checked by CMake at build time and by `__builtin_available` at run time). Its Cholesky (macOS 10.13 and later) tests positive definiteness, as Eigen's LDLT did. Eigen's elsewhere: Linux, Windows, older macOS. `QUCS_FEM_SOLVER=eigen` forces Eigen's, to compare. |
| Keep "analyse once while the pattern stays" | Kept, and more. A Newton's Jacobian whose entries come and go (an avalanche diode's ionization where the field is high) had been analysed 2,264 times in 8,500 factorizations. The analysed pattern is now the union of the patterns seen: a matrix within it is factorized there, its other entries nought. An analysis is kept across problems: the last four by pattern, compared entry by entry, not by hash alone. A sweep's next point reuses it (1.9 s, then 0.06). |
| — | **Below 20,000 unknowns Eigen's.** Accelerate's analysis costs about eight factorizations: at 2,000 unknowns 35 ms, against Eigen's 5 ms for the whole solve, and its threads cost more than such a matrix (617 s of system time in the avalanche test). Measured on a two-unknown convection-diffusion grid: Accelerate's refactorization is 4× Eigen's factorization at 1,800 unknowns and 64× at 500,000; for one solve, analysis included, they cross near 20,000. `QUCS_FEM_ACCELERATE_FROM` sets another limit. |
| — | **A singular matrix said.** Accelerate's LU reported a matrix with a row and column of noughts as factorized, and solved it finite and wrong (a residual of 0.035). Eigen says "structurally singular", as every caller reads it. Each factorization is now checked with one solve of a fixed right-hand side: a backward error over 1e-6 is said singular (it was 0.028 there). That costs about a tenth of the factorization. Nothing is solved after a failed factorization; the result is NaN, which callers report as not finite. Eigen's own solve crashed there. Without a callback, Accelerate stops the program on a parameter error; one is given. |
| The iterative switch at 400,000 unknowns | Kept only where Eigen's simplicial Cholesky is the direct solver. With Accelerate the direct one is taken. |
| — | The complex system's real-equivalent LDLT (faster than Eigen's complex LU) is skipped where Accelerate's complex LU is there. |

**Measured** in Qucs-S, through its tools, on the review's OLED model (681,054 unknowns, one wavelength) and the VCSEL laser example:

| | Eigen | Accelerate |
|---|---|---|
| OLED, one wavelength | 28.2 s | 4.7 s (analysis 1.9 s, factorization 0.15, the check 0.03) |
| OLED, three wavelengths | 85.9 s | 8.6 s: 2.2 s a wavelength after the first |
| VCSEL laser example, 36 points | 74.6 s | 19.8 s |

The answers agree: the OLED field at four points to 1e-12, and the VCSEL's current, power, Q and wavelength to 1e-11. On a large model Accelerate's factorization runs on several threads and is not bitwise reproducible: two identical solves differ in their last digits. A test that compared two runs bit for bit now compares to 1e-12.

## 3. Faster assembly

Once the factorization is fast, the OLED's per-wavelength time is the assembly: evaluating its PML coefficients at each point of integration (2.25 s a wavelength).

- **The Jacobian's nudges evaluate what they change.** Each of the six nudges (u, v and their x and y derivatives) evaluated every coefficient again. Now each domain's expressions are marked once with the nudges that can change them: those reading that component's value or that derivative, or a derived variable or coefficient, whose reads are not known. The rest keep their values, the same numbers bit for bit. 2.28 s a wavelength, from 2.79.
- **The elements' threads take chunks as they finish.** `parallelFor` took one fixed chunk a thread, so a thread whose chunk held the PMLs held up the rest. It now takes four chunks a thread from a shared counter. 2.19 s a wavelength.

What is left is the expressions' evaluation itself.

## 4. Sweep points in parallel

| Recommended | Now |
|---|---|
| Wavelengths, orientations and source positions run concurrently inside one Qucs-S (it took several headless processes) | The Parametric Sweep step's **Points at once** (`"parallel"`, 1 by default): points solved in batches of that many, each on its own threads (its assembly's and its factorization's), their solutions in the sweep's order. Each point holds its own matrices, so its memory is multiplied too; the tooltip says so. The geometry and mesh are made before each batch, one point at a time where a parameter changes them. Progress is told one point at a time, and a stop stops each. The tools say "The sweep: N points, M at once, in X s", X being the sweep's own time; summed, the points' overlapping seconds were more. |
| — | **Where a point starts from the one before, one after another, said.** A semiconductor walks from the last point's solution; a mode step asking for one mode, Follow on and no domains tracked, searches about the last point's mode. Such a sweep runs one point at a time and the log says "one point after another - Semiconductor starts from the point before". (`track`'s default, no domains, is not tracking.) |
| — | **Found doing it:** Accelerate's symbolic LU on several threads at once stopped the program, a SIGSEGV in libSparse, three runs out of three on the OLED sweep at once. Analyses now run one at a time; the others then find the analysis in the cache. The factorizations themselves run at once. |

Measured: the grating example's nine angles (26,488 unknowns each) took 1.08 s one after another, 0.40 s four at once and 0.175 s nine at once, the same reflectances. The OLED's three wavelengths at once took 7.2 s, against 8.4 one after another. Its points each already keep the cores busy, so the gain is small; it is large for models of fewer unknowns.

Tests:

- **test_fem_physics `sweepPointsAtOnce` (new):**
  - six lengths four at once, the same conductances, names and values as one at a time, each its own mesh, the sweep's time;
  - three conductivities three at once on one mesh;
  - a semiconductor's two voltages one after another, said.
- **test_fem_waves `aModeFollowedIsSweptInTurn` (new):** three frequencies at once with Follow off, in turn with it on, the same TE10 index.
- **test_fem_sparse `onThreadsAtOnce` (new):** eight threads analysing and factorizing at once, Accelerate forced, each solve to 1e-12.
- **test_multiphysics_doc:** "3 points, 3 at once".

Nine changes broken on purpose:

- **8 caught:**
  - points one after another whatever asked;
  - a semiconductor, or a followed mode, taken as independent;
  - the default track taken as tracking;
  - the points out of order;
  - no sweep time;
  - the analyses at once (the threads test: a SIGSEGV);
  - the tools' "at once" unsaid.
- **1 not caught:** the tools' seconds summed. On the test's points, of milliseconds, the sum and the sweep's own time differ by timing alone.

The Release suite 127 of 127; the touched tests under the sanitizers, no report.

## 2. Native sources in the waves physics

| Recommended | Now |
|---|---|
| A point or line current source and a dipole in `waves`, so an emission problem in the frequency domain needs no hand-written real-form PDE: half the unknowns, the PML in the physics | Two features of Electromagnetic Waves, Frequency Domain: **Point Dipole** (position; px, py in the plane for Hz, pz out of it for Ez, in C per metre of z; phase) and **Line Current** (position, I, phase; Ez). Their current is a point's: a line current's I δ, a dipole's jω p δ. Ez: −jωµ0 I N_i(r0) on the right-hand side. Hz: [∇×(ε⁻¹J)]_z, tested by parts, jω (ηp × ∇N_i) at r0, η = ε_t⁻¹. The field is complex, the PML is the physics' own, and a sweep, a parallel sweep and the tools take them as any feature. |
| — | **Their power.** ewfd.Psource, −½ Re ∫E·J*, read from the solved field at the source (the current's own field is reactive). ewfd.Psource0 is the same sources' power in a homogeneous medium. In 2D that is the same in every medium: ω µ0 \|I\|²/8 for a line current, µ0 ω³ \|p\|²/8 for a dipole out of the plane and /16 in it. ewfd.Purcell is their ratio. Several sources each take the field of all, so two at one place in antiphase give nothing. |
| — | **With ports.** Ports not fed take what reaches them: ewfd.Pport_<name>, \|b\|² Re β / 2ωµ0 (ε0 for Hz). A fed port or an incoming plane wave alongside a source is refused: the sources' power is of their field alone. |
| — | **Said.** A position outside the physics' domains, and a source the polarization cannot carry, are refused: a line current under Hz; a dipole with no moment in the plane under Hz, or none out of it under Ez. A moment of the other polarization is left out, with a warning. A unit that is not a charge (or a current) gets a warning. So does a source in an absorbing medium or a PML: its near field's absorption, and so its power, grows as the mesh is refined. |
| — | **Found doing it: a dipole on a node.** Its ∇N differs from one triangle to the next. The triangles round the point are each taken by their angle about it, as a small disc of current would be. The power converged, but slowly: in glass, on a node, at hmax 0.05 µm (λ/n 0.5 µm), 4.2 % high. **The mesh is now four times finer about each source** (a size point at a quarter of the size there). That case is now 0.39 %; inside a triangle, 0.02 %. |
| — | **Marked.** The Graphics view draws a circled cross where a chosen source is, and follows its position as it is edited, parameters included. |

**Measured against the analytic answers:**

- **Free space, a box 4 µm across inside a 1 µm PML at λ0 1 µm, hmax 0.05 µm:**
  - a line current: Purcell 0.99999;
  - the same in glass, on a node: 0.99998;
  - a dipole out of the plane: 0.99999;
  - a dipole in the plane: 0.99982;
  - in glass (n 2), on a node, along xy: 1.0039.
- **A parallel-plate guide 10 mm wide at 20 GHz, matched ports at its ends:**
  - a line current midway: 28,452.85 W/m, against ω µ0 \|I\|²/2wβ, 28,452.85;
  - a dipole across it (the TEM mode): 148.7247 W/m, against ω k0 \|p\|²/4ε0w, 148.7272.
  - In both cases the two ports take the source's power to 1e-7, half each.
- **The dipole in the plane's power, by mesh, before the refinement about it:**
  - inside a triangle: 1.1 % at hmax 0.08 µm, 0.03 % at 0.04, 0.005 % at 0.02;
  - in the guide: 0.2 %, 0.09 %, 4e-5 at hmax 0.5, 0.25 and 0.125 mm.
  - Ez's line current: 0.01 % at 0.08 µm.

**The review's OLED model on the native physics.** I rebuilt it from the same geometry, mesh and material tables, with an x dipole at (0, 330 nm) (the scratch files, not shipped):

- **Its side PMLs are their own domains.** The physics' PML is by domain, so each layer is cut at x = ±15.3 µm, in a gap between the pattern's pillars.
- **The materials are given as n − jκ** from the same tables.

| | Hand-written real form | Native waves |
|---|---|---|
| Unknowns | 681,054 real | 341,275 complex |
| One wavelength | 4.08 s | 1.22 s |
| Three wavelengths | 8.56 s (2.18 s each after the first) | 2.08 s (0.45 s each) |

Both use Accelerate's LU; with Eigen's, the hand-written model took 28.2 s a wavelength.

**The same field.** At eight points, the hand-written field over the native one is 1/(jωp): the hand-written source is a unit current moment. It agrees to 0.8 % in magnitude and 1° in phase. One point, 3 µm from the side PML, is 8 % off; there the two PMLs start and grow differently.

Tests:

- **test_fem_waves `pointSources` (new):**
  - five sources against their homogeneous power;
  - the mesh about each a quarter of hmax;
  - a dipole pz the same as a line current jω pz;
  - a line current and a dipole at one place: cancelled at 90°, twice the power of each at 270°, the same at 0°.
- **test_fem_waves `pointSourcesInAGuide` (new):** both guides against the analytic powers and the ports' share; a fed port refused.
- **test_fem_waves `pointSourceRefusals` (new):**
  - six refusals;
  - the other polarization's moment, a wrong unit and a source in the PML said.
- **test_multiphysics_doc `aPointSourceMarkedAndSolved` (new):**
  - the mark where the chosen dipole is, drawn, and gone when another node is chosen;
  - moved with its parameter;
  - solved by the tools: ewfd.Psource, Psource0, Purcell.

**Timed under the sanitizers before the commit.** With Eigen's LU, as CI's Linux has it, the first `pointSources` ran past QtTest's 300 s: some 100 s a solve at hmax 0.08 µm in the 6 µm box. A sanitized build now takes a box half as wide at hmax 0.1 µm, and leaves out one case and the two sources at 0°. The guide takes hmax 0.5 mm there, and its refinement check meshes without solving. The three functions now take 36, 21 and 7 s, with either solver.

Runs:

- **The Release suite:** 127 of 127; test_fem_waves and test_multiphysics_doc again after the sanitized cut.
- **Under the sanitizers, no report:** the four new functions.
- **The lints pass.**

Nineteen changes broken on purpose, all caught (one after its test was made stricter):

- **Caught at once:**
  - the line current's sign;
  - the in-plane power without the conjugate;
  - one triangle at a node, not each by its angle (narrowly: 1.0053, against the test's 0.5 %);
  - no refinement about a source;
  - the refinement the whole model's size, not its domain's;
  - the in-plane homogeneous power /8;
  - ε0 and µ0 swapped in a port's power;
  - a fed port, or an incoming plane wave, allowed;
  - the absorbing medium unsaid;
  - the position not scaled to metres;
  - a dipole's current ω pz, not jω pz;
  - the phase ignored;
  - the other polarization's moment unsaid;
  - Hz without a moment in the plane allowed;
  - a wrong unit unsaid;
  - the sources' right-hand side not added;
  - the mark not set.
- **Caught after its test was made stricter:** the mark not drawn. Choosing another node also changed the view's highlight, so the two images differed anyway. The test now compares the view with and without the mark alone, near it and away from it.

## Tests

- **test_fem_sparse (new):**
  - `theLuOnEachBackend`: a real and a complex system solved to 1e-12 by Accelerate, by Eigen below the size asked, and with `QUCS_FEM_SOLVER=eigen`; by default 20,000 unknowns, a 3,200-unknown system Eigen's;
  - `aPatternThatChanges`: patterns with one extra entry or another, alternated: three analyses (a union), each solve to 1e-12; another size; a second object of the same pattern analyses nothing (the cache);
  - `aSingularMatrixIsSaid`: said on both backends, with a message, and its solve not finite;
  - `theCholesky`: a positive definite system solved, an indefinite one refused, on both backends.
- **The FEM tests with Accelerate at every size** (`QUCS_FEM_ACCELERATE_FROM=0`): 22 of 22, after `metalWorkFunction` compared two solves to 1e-12, not bit for bit.

## Runs

- **The Release suite:** 127 of 127.
- **The FEM tests with Accelerate at every size:** 22 of 22.
- **Under the sanitizers, no report:** test_fem_sparse, test_fem_pde, test_fem_physics, test_fem_engine and test_fem_magnetics whole; functions of test_fem_waves, test_fem_semiconductor, test_fem_laser and test_fem_quantum.
  - They found an Eigen assertion: the wrapper read `info()` after an analysis alone, which Eigen allows only after a factorization. Release skips the check. CI's Debug job would have stopped at every solve.
- **The lints pass.**

Fourteen changes broken on purpose:

- **10 caught:**
  - a pattern analysed alone, not with the one before;
  - no analysis kept for the next;
  - a singular matrix said factorized;
  - solved after a failure;
  - Accelerate at any size;
  - `QUCS_FEM_SOLVER` read once;
  - Eigen's Cholesky of an indefinite matrix taken;
  - Accelerate's Cholesky failure unread;
  - the nudges evaluating nothing again;
  - a derived variable taken as not reading the unknowns.
- **4 not caught:**
  - **No error callback:** no test makes a parameter error; the wrapper makes no malformed matrices.
  - **Accelerate's default pivot tolerance (0.01, not 0.1):** every test's accuracy holds either way.
  - **Every expression evaluated again at each nudge:** the same numbers, slower.
  - **One chunk a thread:** the same numbers, slower.
