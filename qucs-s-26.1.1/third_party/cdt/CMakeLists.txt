# CDT 2.0.0, constrained Delaunay triangulation and Ruppert's refinement
# (README.qucs.md): the mesher of the multiphysics solver (qucs/multiphysics).
# Header-only, included as <CDT.h>.

add_library(cdt INTERFACE)
# (SYSTEM: its headers' warnings are not ours.)
target_include_directories(cdt SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
