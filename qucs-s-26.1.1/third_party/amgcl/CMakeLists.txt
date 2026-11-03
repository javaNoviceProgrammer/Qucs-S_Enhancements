# AMGCL 1.5.0, algebraic multigrid (README.qucs.md): the iterative solve of
# the multiphysics solver's 3D models (qucs/multiphysics). Header-only,
# included as <amgcl/...>, without Boost.

add_library(amgcl INTERFACE)
# (SYSTEM: its headers' warnings are not ours.)
target_include_directories(amgcl SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(amgcl INTERFACE AMGCL_NO_BOOST)
