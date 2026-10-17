# The multiphysics solver's engine (fem_*.cpp): the model, its geometry
# (drawings imported too; Lumerical's MQW projects, through a reader of
# HDF5), mesh, elements, solver - its scalar physics and solid mechanics -
# results and materials - Qt Core only, no widgets;
# on Eigen and CDT (header-only, third_party/) and the Clipper inside gdstk.
# The window's part (the Multiphysics panel, a model's tab) is in qucs-core.

set(FEM_SRCS
  fem_expr.cpp
  fem_model.cpp
  fem_geometry.cpp
  fem_mesh.cpp
  fem_fe.cpp
  fem_solver.cpp
  fem_sparse.cpp
  fem_scalar.cpp
  fem_elastic.cpp
  fem_magnetic.cpp
  fem_edge.cpp
  fem_waves.cpp
  fem_semiconductor.cpp
  fem_rays.cpp
  fem_quantum.cpp
  fem_optics.cpp
  fem_line.cpp
  fem_pde.cpp
  fem_import.cpp
  fem_hdf5.cpp
  fem_lumerical.cpp
  fem_lumerical_export.cpp
  fem_lumerical_import.cpp
  fem_lumerical_script.cpp
  fem_results.cpp
  fem_export.cpp
  fem_materials.cpp
  fem_material_plot.cpp
  fem_tensor.cpp
)

add_library(fem STATIC ${FEM_SRCS})
set_target_properties(fem PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF POSITION_INDEPENDENT_CODE ON)
target_compile_features(fem PUBLIC cxx_std_20)
target_include_directories(fem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# (Clipper 6.4.2, built into gdstk: its header from there.)
target_include_directories(fem SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/gdstk/external)
target_link_libraries(fem PUBLIC Qt6::Core eigen cdt PRIVATE gdstk)
# M_PI and its kin in <cmath> under -std=c++20 with MinGW and MSVC too.
target_compile_definitions(fem PUBLIC _USE_MATH_DEFINES)
# The sparse direct factorizations (fem_sparse.cpp): on macOS Accelerate's -
# supernodal, multithreaded; its Cholesky from macOS 10.13, its LU from 15.5
# where the SDK has it (Xcode 16.4 and later), told at run time; Eigen's
# elsewhere and before.
if(APPLE)
  find_library(QUCS_ACCELERATE_FRAMEWORK Accelerate)
  if(QUCS_ACCELERATE_FRAMEWORK)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LIBRARIES ${QUCS_ACCELERATE_FRAMEWORK})
    check_cxx_source_compiles("
      #include <Accelerate/Accelerate.h>
      int main() { SparseFactorization_t t = SparseFactorizationLU; return int(t) == 80 ? 0 : 1; }" QUCS_ACCELERATE_HAS_LU)
    unset(CMAKE_REQUIRED_LIBRARIES)
    target_link_libraries(fem PUBLIC ${QUCS_ACCELERATE_FRAMEWORK})
    set_property(SOURCE fem_sparse.cpp APPEND PROPERTY COMPILE_DEFINITIONS QUCS_ACCELERATE)
    if(QUCS_ACCELERATE_HAS_LU)
      set_property(SOURCE fem_sparse.cpp APPEND PROPERTY COMPILE_DEFINITIONS QUCS_ACCELERATE_LU)
    endif()
    message(STATUS "Multiphysics sparse solver: Accelerate (its LU: ${QUCS_ACCELERATE_HAS_LU})")
  endif()
endif()
