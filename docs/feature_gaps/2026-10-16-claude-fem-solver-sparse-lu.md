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

## 4 and 2

Each in a commit of its own, after this one; written up below as they are done.

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
