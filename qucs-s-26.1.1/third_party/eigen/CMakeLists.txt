# Eigen 5.0.1, header-only linear algebra (README.qucs.md): the sparse
# matrices and direct solvers of the multiphysics solver (qucs/multiphysics).
# Its headers as a library of none, included as <Eigen/Sparse>.

add_library(eigen INTERFACE)
# (SYSTEM: its headers' warnings are not ours.)
target_include_directories(eigen SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
