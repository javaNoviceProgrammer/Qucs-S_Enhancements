# MUMPS and STRUMPACK: the multiphysics solver's factorizations on many threads where Accelerate is not (2026-11-09)

The user: "Looks like Eigen solver on my linux workspace is super slow since it only runs in one thread eventhough I have 16
cores. What other solvers can we use on linux to take advantage of parallelism?" Then, given a survey with MUMPS first and
STRUMPACK second: "how about both? Let the user choose in the "Settings..." window of each Multiphysics and Multiphysics 3D."

## Where it stood

`fem_sparse` factorized by Apple's Accelerate (supernodal, multithreaded) on macOS, from 20,000 unknowns, and by Eigen's
`SparseLU` (COLAMD) and `SimplicialLDLT` everywhere else - one thread, an ordering made for 2D, and no LDLT of a complex
symmetric matrix: every full-wave system (`SparseComplexLdlt`) went through a complex LU. On CI's macOS 15 (no Accelerate LU)
the wire through PMLs that Accelerate's LDLT solves in 43 s did not finish in 300 s at 18.6 GB.

## The survey (what was weighed)

| Library | Real / complex LU | Complex symmetric LDLT | Threads | Licence against the GPL | x86-64 / arm64 / MSYS2 | Packages |
|---|---|---|---|---|---|---|
| oneMKL PARDISO | yes / yes | yes (mtype 6) | OpenMP, TBB | Intel's, proprietary: not in a GPL release | x86-64 only, not in MSYS2 | Debian non-free, Intel's |
| **MUMPS 5.9** | yes / yes | **yes**, threshold pivoting with delayed pivots; SPD too | OpenMP + BLAS | CeCILL-C (LGPL-like; the FSF calls it GPL-incompatible) | all; MSYS2 5.9.1 | Debian/Ubuntu `libmumps-seq-dev` (no OpenMP), Fedora `MUMPS-openmp` |
| **STRUMPACK 8.1** | yes / yes | no (an LU) | OpenMP tasks | BSD-3 | all; MSYS2 8.1.0 | none |
| PaStiX 6.4 | yes / yes | static pivoting only | its own scheduler | LGPL-3 | MSYS2 6.4.0 | not Debian, Fedora |
| SuperLU_MT 4 | yes / yes | no | pthreads, OpenMP | BSD | MSYS2 4.0.2 | Fedora |
| CHOLMOD, UMFPACK, ParU | Cholesky only; LU through BLAS; ParU real only | no | BLAS; ParU OpenMP | GPL-2+/3+ | all | everywhere (ParU from Ubuntu 24.04) |

MUMPS is the one free library with all four kinds of solve the solver has (a real LU for semiconductors' Jacobians that span
30 decades, a complex LU, a Cholesky, a complex symmetric LDLT with real pivoting). STRUMPACK has the clean licence and
threads well, but has no LDLT. The user took both, each chosen in the Settings.

## What it is

- **The choice.** `DirectSolver` has two more: `Mumps`, `Strumpack` (`QUCS_FEM_SOLVER=mumps`, `strumpack`), the setting
  `Multiphysics/solver` `mumps`, `strumpack`. Both Settings windows (Multiphysics, Multiphysics 3D) show the same five choices
  from one function (`MultiphysicsSettingsDialog::addSolverChoices`): Automatic, named by what it takes in this build
  ("Automatic (MUMPS from 20,000 unknowns, Eigen below)" on Linux), Eigen at every size, Accelerate at every size, MUMPS from
  20,000 unknowns, STRUMPACK from 20,000 unknowns - a library the build has not, " - not in this build" after its name. The
  status under the box says what factorizes for the choice ("MUMPS's LU, LDLT and Cholesky from 20,000 unknowns, Eigen's below
  (MUMPS 5.9.1)."), and why not where a choice cannot be had ("Eigen's LU and Cholesky (LDLT) at every size: this build has no
  MUMPS (…)").
- **Which factorizes** (`luBackend()`, `choleskyBackend()`, and `…For(DirectSolver)` for the Settings' status): Automatic takes
  Accelerate's where this macOS has it, else MUMPS's, else STRUMPACK's, else Eigen's; a library chosen is taken where the
  build has it, Eigen's else. `acceleratedLu()` and `acceleratedCholesky()` keep their meaning (Accelerate's is the one).
- **Each class.**
  - `SparseLu` (real, complex): MUMPS's LU (`SYM=0`, METIS's ordering, its own scaling and maximum transversal) or STRUMPACK's
    (METIS's nested dissection, MC64's matching, no compression). Its pattern P is kept as Accelerate's is, analysed at the first
    factorization (with its values - both libraries scale and permute by them) and again only when an entry falls outside P (a
    Newton's Jacobian whose entries come and go: three analyses for the test's six patterns, as Accelerate's). The check of each
    factorization (`checkSolve`: the backward error, and the condition where asked) is the same as the others'.
  - `SparseCholesky`: MUMPS's LDLT without pivoting (`SYM=1`; a negative pivot - `INFOG(12)` - is "not positive definite", the
    caller then takes an LU; a pivot of nought is singular to rounding, said, as Accelerate's), or STRUMPACK's LU in its place
    (any matrix it can factorize; named "STRUMPACK's LU").
  - `SparseComplexLdlt` (full-wave): MUMPS's LDLT (`SYM=2`, threshold pivoting) where MUMPS is the LU's backend, its factors'
    estimate (`INFOG(17)`) put against the 3D memory limit before it factorizes, as Accelerate's analysis's is; else a SparseLu
    (STRUMPACK's LU where chosen).
- **Sizes.** `mumpsFrom()`, `strumpackFrom()`: 20,000 unknowns, 50,000 for one of many solved at once (Accelerate's figures,
  which the measurements below bear out for 2D: MUMPS's LU and Eigen's take the same time at 20,000; in 3D MUMPS's is already 2
  to 10 times quicker at 8,000). `QUCS_FEM_MUMPS_FROM`, `QUCS_FEM_STRUMPACK_FROM` another (0: every size).
- **Calls one at a time.** Each library is called under a lock of its own (`fem_sparse_external.cpp`): Debian's MUMPS is built
  without OpenMP, and gfortran then keeps a routine's large local arrays static - two instances on two threads would share
  them. A sweep's points solved at once take their turns, each factorization on all the cores - as Accelerate's do.
- **The same to the last bit.** MUMPS's threads on the elimination tree's subtrees (`ICNTL(48)`, on by default in MUMPS since
  5.7) are scheduled as they come free, and its sums then come out in another order from run to run: test_fem3_eme's MMI solved
  on 1 and on 8 threads (each call on all the cores either way) differed in the last digits. Off here, the solves are the same to
  the last bit, at 15 to 50 % of the speed (a 3D Laplacian's Cholesky of 64,000 unknowns 0.62 s against 0.48 on 8 threads; the
  2D LU of 51,200 0.32 against 0.21). `QUCS_FEM_MUMPS_TREE=1` the quicker.
- **Threads.** `QUCS_FEM_THREADS` the threads one call may take (MUMPS's `ICNTL(16)`; STRUMPACK's `omp_set_num_threads` on the
  calling thread before each call); OpenMP's own (`OMP_NUM_THREADS`, all the cores) else. STRUMPACK's calls run with one active
  level of parallelism, and on macOS on 8 threads at most - see below.
- **The log.** Each solve's line names it: "Solved … with MUMPS's LU", "MUMPS's LDLT", "MUMPS's Cholesky", "STRUMPACK's LU"
  (`solverUseText`); a 3D solve's "…, MUMPS's Cholesky (METIS order)", "…, MUMPS's LU (Newton's matrix)". Claude's `fem_model`
  description names them. A 3D heat or electrostatics model with many right-hand sides factorizes by itself where the Cholesky
  is a multithreaded one (`threadedCholesky()`: MUMPS's and STRUMPACK's too, not Accelerate's alone).

## Building

- **CMake** (`qucs/multiphysics/CMakeLists.txt`), both optional, Eigen's the fallback: `QUCS_WITH_MUMPS`, `QUCS_WITH_STRUMPACK`
  (on). MUMPS: pkg-config's `mumps-dto` and `mumps-zto` (MSYS2), else `dmumps_c.h` and the sequential libraries under
  `MUMPS_ROOT` or the system's - Debian's `lib*mumps_seq` first (its `lib*mumps` are MPI's), MUMPS's own build's `lib*mumps`
  -, `MUMPS_EXTRA_LIBRARIES` what a static build needs besides (METIS, BLAS, gfortran, gomp). STRUMPACK: its CMake package
  (`STRUMPACK_DIR`). They are compiled in a library of their own (`fem_external`): STRUMPACK's target brings OpenMP's compile
  options, which would thread Eigen's and AMGCL's code in the rest of `fem`. `QUCS_SPARSE_SOLVERS_REQUIRED` (off; on in CI and
  the releases) fails the configure where one is not found, rather than ship Eigen's alone.
- **`scripts/ci/build-sparse-solvers.sh <prefix> [mumps] [strumpack]`**: both from their sources, each checked against its
  SHA-256 (MUMPS 5.9.1 from mumps-solver.org, STRUMPACK 8.1.0 from GitHub), static libraries of position-independent code -
  MUMPS's Makefile gives its shared libraries no SONAME, so a program linked to them would ask for this build's paths -, with
  OpenMP, METIS and OpenBLAS's OpenMP build (its pthreads build would start threads inside the solvers' OpenMP ones).
- **CI** (`ci.yml`): the Linux job installs `libmumps-seq-dev` (Ubuntu 24.04's 5.6.2: Automatic there is MUMPS, so every
  multiphysics test past 20,000 unknowns runs on it), `libopenblas-openmp-dev`, `libmetis-dev`, `gfortran`, builds STRUMPACK
  with the script (cached while the script is unchanged) and configures with both required. The Windows job installs MSYS2's
  `mumps` and `strumpack` (and `pkgconf`).
- **Releases** (`release.yml`): the Linux AppImages (Ubuntu 22.04 x86-64, 24.04 arm64) build both with the script (gcc-12,
  gfortran-12; cached) and link them; linuxdeploy copies what they need of the system's (OpenBLAS, METIS, libgfortran, libgomp).
  The Windows builds (ucrt64, clangarm64) take MSYS2's; `windows-bundle.sh` bundles their DLLs with the rest and checks them as a
  PC without MSYS2 runs them. The macOS releases are unchanged (Accelerate).

## Measured

`test_fem_sparse`'s `benchmarkAgainstEigen` (run with `QUCS_FEM_BENCH=1`, `full` for the larger sizes): each analysed,
factorized and solved once, its residual checked. On this Mac (M2 Ultra, 16 performance and 8 efficiency cores), MUMPS and
STRUMPACK built here with Homebrew's GCC 16, OpenBLAS and libomp, `OMP_NUM_THREADS=8`:

| Problem | Unknowns | Eigen | Accelerate | MUMPS | STRUMPACK |
|---|---|---|---|---|---|
| 2D convection-diffusion, LU | 5,000 | 0.011 s | 0.038 | 0.023 | 0.022 |
| | 20,000 | 0.063 | 0.081 | 0.079 | 0.092 |
| | 51,200 | 0.235 | 0.149 | 0.210 | 0.251 |
| 3D Laplacian, Cholesky | 8,000 | 0.104 (LDLT) | 0.034 | 0.049 | 0.050 (LU) |
| | 27,000 | 1.149 | 0.128 | 0.184 | 0.250 |
| | 64,000 | 10.106 | 0.367 | 0.479 | 0.490 |
| 3D Helmholtz with a lossy edge, complex symmetric | 8,000 | 0.478 (LU) | 0.036 (LDLT) | 0.200 (LDLT) | 0.069 (LU) |
| | 27,000 | 5.870 | 0.169 | 0.599 | 0.431 |

So in 3D, where Eigen's single thread and COLAMD hurt most, MUMPS's and STRUMPACK's are 10 to 21 times Eigen's here, within
1.3 to 3.5 times Accelerate's; in 2D they pay below 20,000 unknowns, which is what the sizes are for. On all 24 threads MUMPS's
3D figures were 1.3 to 2.5 times slower than on 8 (the efficiency cores), STRUMPACK's could be hundreds of times slower:

- **STRUMPACK on macOS's libomp.** Sampled, the threads sat in libomp's waits (`__kmp_wait_4`, its bakery lock,
  `__kmpc_critical_with_hint`) around STRUMPACK's fronts' memory pool (`VectorPool`, an OpenMP critical section). The 2D LU of
  20,000 unknowns took 0.09 s on 8 threads and 34 s on 24; capped at 8 inside the call it still varied from run to run (0.3 to
  168 s), less with one active level (`omp_set_max_active_levels(1)`, now set) - and this Mac mixes three OpenMP runtimes
  (libomp for STRUMPACK and OpenBLAS, gfortran's libgomp for MUMPS) on unequal cores. The Linux builds have one runtime
  (libgomp) and GCC throughout; how STRUMPACK scales there is what the Linux benchmark below is for. The macOS releases carry
  neither library.

## For the user's 16-core Linux machine

With the script (needs `gcc g++ gfortran make cmake ninja-build curl libopenblas-openmp-dev libmetis-dev`):

```bash
bash scripts/ci/build-sparse-solvers.sh ~/sparse-solvers
cmake -S qucs-s-26.1.1 -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMUMPS_ROOT=$HOME/sparse-solvers -DMUMPS_EXTRA_LIBRARIES="metis;openblas;gfortran;gomp" -DSTRUMPACK_DIR=$HOME/sparse-solvers/lib/cmake/STRUMPACK -DQUCS_SPARSE_SOLVERS_REQUIRED=ON
cmake --build build --target test_fem_sparse
QUCS_FEM_BENCH=full ctest --test-dir build -R '^test_fem_sparse$' -V
OMP_NUM_THREADS=8 QUCS_FEM_BENCH=full ctest --test-dir build -R '^test_fem_sparse$' -V
```

Its table says whether 20,000 is the right size there, and how STRUMPACK scales on 16 cores.

## Tests

- `test_fem_sparse` (each skipped, saying why, where the build has not the library): `eachLibrarysLu` (MUMPS, STRUMPACK: real
  and complex to 1e-12, several right-hand sides, named and counted, a changing pattern analysed three times, another size, a
  singular matrix said and its solve no number, Eigen's below its size, one of many at once from 50,000), `eachLibrarysCholesky`
  (MUMPS's refuses an indefinite matrix by its negative pivot, STRUMPACK's solves it; a potential left floating singular to
  rounding for both), `aComplexSymmetricMatrix` (MUMPS's LDLT, its estimate and a memory limit under it refused;
  STRUMPACK's LU; each against Eigen's LU to 1e-10), `eachLibraryOnThreadsAtOnce` (eight threads, the library called one call
  at a time), `aLibraryNotInTheBuild` (Eigen's then, why said; the environment's choices; Automatic's order), `singularToRounding`
  with MUMPS and STRUMPACK beside Eigen and Accelerate, and `benchmarkAgainstEigen` (skipped unless `QUCS_FEM_BENCH`).
- `test_multiphysics_doc::theSettingsHaveTheSolver`: five choices, MUMPS's and STRUMPACK's items and statuses for the build,
  MUMPS chosen and kept as `mumps`, a solve saying "with MUMPS's Cholesky" where the build has it, Automatic on Linux saying it
  too. `test_multiphysics3d_doc`: the 3D Settings' same five.
- Here (macOS 27, Release, both libraries built): `test_fem_sparse`, `test_multiphysics_doc` and `test_multiphysics3d_doc`
  pass. The 43 multiphysics tests (`test_fem*`, `test_multiphysics*`) with `QUCS_FEM_SOLVER=mumps` - Linux's Automatic, from
  20,000 unknowns -: 41 passed; `test_multiphysics3d_doc` failed only because the environment's choice is over the one its
  Settings make (as it says), and `test_fem3_eme`'s MMI on 1 and 8 threads differed in the last digits - MUMPS's tree
  parallelism, now off (above); with it off `test_fem3_eme` passes, in 292 s against Eigen's 501 s (the cuts' solves taking
  turns on all the cores, against Eigen's side by side on one each). A build with Accelerate, MUMPS and STRUMPACK all off
  (`-DQUCS_ACCELERATE_FRAMEWORK=OFF -DQUCS_WITH_MUMPS=OFF -DQUCS_WITH_STRUMPACK=OFF`, Linux's without them):
  `test_fem_sparse` and `test_multiphysics_doc` pass, their Accelerate checks now asked of the build, not of `__APPLE__`.
  The four `scripts/ci/check-*.py` lints pass.

## Not done

- **Linux and Windows not run here.** CI's Linux job is the first run of MUMPS as Automatic under the sanitizers; its Windows job
  the first link against MSYS2's packages; the release workflow's AppImage and Windows bundle the first bundling.
- **Tests skipped where Accelerate's LU is not** (`test_fem3_wave`, `test_fem3_wavetd`, some of `test_fem3_eme`) still look for
  Accelerate's (`acceleratedLuHindrance()`); with MUMPS they could run on Linux and Windows.
- **macOS CI** (macOS 15: no Accelerate LU) could take MUMPS from Homebrew-built sources the same way, and run the full-size
  wave solves it now skips.
- **The licence.** MUMPS's CeCILL-C is LGPL-like; the FSF lists it as incompatible with the GPL (no clause relicensing it under
  the GPL), while Debian ships GPL programs that load it (GetDP through PETSc). Releasing the AppImages and Windows builds with
  it is the user's call; STRUMPACK (BSD-3) has no such question.
