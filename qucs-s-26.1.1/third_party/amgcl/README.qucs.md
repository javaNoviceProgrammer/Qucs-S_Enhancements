# AMGCL in Qucs-S

[AMGCL](https://github.com/ddemidov/amgcl), algebraic multigrid in C++, for
the multiphysics solver's 3D models (`qucs/multiphysics/fem_amg.cpp`):
conjugate gradients preconditioned by smoothed aggregation, the iterative
solve of a 3D heat, electrostatics or currents model, whose iterations stay
nearly constant as the mesh is refined where an incomplete Cholesky's grow.

- **Version**: 1.5.0 (3 August 2026): `amgcl-1.5.0.tar.gz` from GitHub
  (codeload of the tag), SHA-256
  `6794cab752f0f15bc40db9c8a08a5ad761adf7d74b3d6652b9e2d4bf7f19ab31`.
- **Licence**: MIT (`LICENSE.md`), which may be combined with the GPL.
- **What is here**: `amgcl/` of that release, unchanged, but for its `mpi/`
  folder and `perf_counter/mpi_aggregator.hpp`, which need MPI; not its
  tests, examples, Python, Fortran or Delphi bindings, documentation or build
  files. `CMakeLists.txt` is ours: an interface library of the headers, with
  `AMGCL_NO_BOOST` defined (its parameters plain structs, no Boost).
- **Header-only**: nothing is compiled or linked, so the packages carry no
  library of it. Without OpenMP (the build has none) its builtin backend runs
  on one thread.

To take a newer version: copy its `amgcl/` over `include/amgcl/` (leaving out
`mpi/` and `perf_counter/mpi_aggregator.hpp` again), and run the `test_fem3_*`
and `test_fem_amg` tests, under the sanitizers too.
