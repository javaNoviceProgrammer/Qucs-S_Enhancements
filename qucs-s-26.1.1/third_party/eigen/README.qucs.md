# Eigen in Qucs-S

[Eigen](https://gitlab.com/libeigen/eigen), the C++ template library for
linear algebra, for the multiphysics solver (`qucs/multiphysics`): its sparse
matrices, the direct solvers (`SimplicialLDLT`, `SparseLU`) and the iterative
ones (`ConjugateGradient`, `BiCGSTAB` with incomplete factorisations).

- **Version**: 5.0.1 (11 November 2025), `eigen-5.0.1.tar.gz` from GitLab,
  SHA-256 `e9c326dc8c05cd1e044c71f30f1b2e34a6161a3b6ecf445d56b53ff1669e3dec`.
- **Licence**: MPL 2.0 (`COPYING.MPL2`), which may be combined with the GPL.
  A few files hold code under the BSD (`COPYING.BSD`) and Apache 2.0
  (`COPYING.APACHE`, `Eigen/src/Core/arch/Default/BFloat16.h`) licences, as
  `COPYING.README` says; Apache 2.0 code goes with the GPL version 3, and
  Qucs-S is GPL version 2 or later.
- **What is here**: the `Eigen/` folder of that release and its `COPYING.*`
  files, unchanged; not its `unsupported/` modules, tests, documentation or
  build files. `CMakeLists.txt` is ours: an interface library of the headers.
- **Header-only**: nothing is compiled or linked, so the packages carry no
  library of it.

To take a newer version: copy its `Eigen/` and `COPYING.*` over these, and
run the `test_fem_*` tests, under the sanitizers too.
