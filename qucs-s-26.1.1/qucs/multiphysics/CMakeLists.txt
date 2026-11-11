# The multiphysics solver's engine (fem_*.cpp): the model, its geometry
# (drawings imported too; Lumerical's MQW projects, through a reader of
# HDF5), mesh, elements, solver - its scalar physics and solid mechanics -
# results and materials - Qt Core only, no widgets;
# on Eigen, CDT and AMGCL (header-only, third_party/) and the Clipper inside
# gdstk. In 3D (fem3_*.cpp): layered geometry, the swept mesh, prisms, and
# heat, electrostatics and currents solved by multigrid (fem_amg.cpp).
# The window's part (the Multiphysics panel, a model's tab) is in qucs-core.
# The optimizers (optimizer_core.cpp) are here because this is the library
# below both of their users: a model's optimization (fem_optimize.cpp) and,
# in qucs-core, an optimization component's (optimization.cpp).

set(FEM_SRCS
  fem_expr.cpp
  fem_model.cpp
  fem_geometry.cpp
  fem3_geometry.cpp
  fem_mesh.cpp
  fem3_mesh.cpp
  fem_modeprofile.cpp
  fem_fe.cpp
  fem3_fe.cpp
  fem_solver.cpp
  fem3_solver.cpp
  fem_sparse.cpp
  fem_amg.cpp
  fem_scalar.cpp
  fem3_scalar.cpp
  fem3_solid.cpp
  fem3_magnetic.cpp
  fem3_semiconductor.cpp
  fem3_results.cpp
  fem3_export.cpp
  fem3_estimate.cpp
  fem3_eme.cpp
  fem3_wave.cpp
  fem3_wavetd.cpp
  fem_elastic.cpp
  fem_magnetic.cpp
  fem_edge.cpp
  fem_waves.cpp
  fem_semiconductor.cpp
  fem_rays.cpp
  fem_quantum.cpp
  fem_exciton.cpp
  fem_optics.cpp
  fem_line.cpp
  fem_pde.cpp
  fem_process.cpp
  fem_process_dopants.cpp
  fem_process_ions.cpp
  fem_process_crystal.cpp
  fem_process_results.cpp
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
  optimizer_core.cpp
  fem_optimize.cpp
  fem_adapt.cpp
)

add_library(fem STATIC ${FEM_SRCS})
set_target_properties(fem PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF POSITION_INDEPENDENT_CODE ON)
target_compile_features(fem PUBLIC cxx_std_20)
# The crystal implant's electron density (fem_process_crystal.cpp): silicon's
# radial table, the review's LDA atom's (no solver at run time). A static
# library's resources reach its users as an object library (Qt 6).
qt_add_resources(fem "fem_crystal_data" PREFIX "/multiphysics/crystal" BASE data FILES data/si_density_cai.txt)
target_include_directories(fem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# (Clipper 6.4.2, built into gdstk: its header from there.)
target_include_directories(fem SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/gdstk/external)
target_link_libraries(fem PUBLIC Qt6::Core eigen cdt PRIVATE gdstk amgcl)
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
    # Weakly: its LU's and complex factorizations' functions are macOS
    # 15.5's, called only where __builtin_available finds them - but their
    # headers mark none of the internal ones, so a strong link made each a
    # symbol the program needs to start: built for 13.0, the release would
    # not start before 15.5 ("Symbol not found:
    # __SparseDestroyOpaqueNumeric_Complex_Double", an Intel Mac on 13.5).
    # Weak, those not there are null, never called. (Its Cholesky's, macOS
    # 10.13's, are there.) test_macos_accelerate_weak checks the program.
    target_link_libraries(fem PUBLIC "-weak_framework Accelerate")
    set_property(SOURCE fem_sparse.cpp APPEND PROPERTY COMPILE_DEFINITIONS QUCS_ACCELERATE)
    if(QUCS_ACCELERATE_HAS_LU)
      set_property(SOURCE fem_sparse.cpp APPEND PROPERTY COMPILE_DEFINITIONS QUCS_ACCELERATE_LU)
    endif()
    message(STATUS "Multiphysics sparse solver: Accelerate (its LU: ${QUCS_ACCELERATE_HAS_LU})")
  endif()
endif()

# MUMPS's and STRUMPACK's sparse direct factorizations (fem_sparse_external.cpp),
# each where found: multithreaded (OpenMP) where Accelerate is not - Linux,
# Windows, an older macOS -, chosen in Multiphysics > Settings… (Automatic:
# MUMPS's, else STRUMPACK's, where Accelerate's is not); Eigen's where
# neither is (docs/feature_gaps/2026-11-09-linux-parallel-solvers.md). In a
# library of their own: STRUMPACK's target brings OpenMP's compile options,
# which would thread Eigen's and AMGCL's code in the rest of fem.
#   MUMPS: pkg-config's mumps-dto and mumps-zto (MSYS2's OpenMP build), else
#     dmumps_c.h and the sequential libraries - Debian's and Ubuntu's
#     libmumps-seq-dev (lib*mumps_seq), or MUMPS's own build (lib*mumps) -
#     under MUMPS_ROOT or the system's; MUMPS_EXTRA_LIBRARIES what a static
#     build of it needs besides (METIS, BLAS, the Fortran and OpenMP
#     runtimes).
#   STRUMPACK: its CMake package (STRUMPACK_DIR, CMAKE_PREFIX_PATH).
option(QUCS_WITH_MUMPS "Factorize the multiphysics solver's matrices by MUMPS where it is found" ON)
option(QUCS_WITH_STRUMPACK "Factorize the multiphysics solver's matrices by STRUMPACK where it is found" ON)
# (CI and the releases: a library not found fails the configure, not a
# release that factorizes by Eigen's alone.)
option(QUCS_SPARSE_SOLVERS_REQUIRED "Fail where MUMPS or STRUMPACK, asked for, is not found" OFF)
set(MUMPS_EXTRA_LIBRARIES "" CACHE STRING "Libraries a static MUMPS needs besides its own (METIS, BLAS and LAPACK, gfortran, OpenMP)")
add_library(fem_external STATIC fem_sparse_external.cpp)
set_target_properties(fem_external PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF POSITION_INDEPENDENT_CODE ON)
target_compile_features(fem_external PUBLIC cxx_std_20)
target_link_libraries(fem_external PRIVATE Qt6::Core)
target_link_libraries(fem PRIVATE fem_external)
set(QUCS_MUMPS_FOUND FALSE)
if(QUCS_WITH_MUMPS)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(QUCS_MUMPS_D QUIET IMPORTED_TARGET mumps-dto)
    pkg_check_modules(QUCS_MUMPS_Z QUIET IMPORTED_TARGET mumps-zto)
    if(QUCS_MUMPS_D_FOUND AND QUCS_MUMPS_Z_FOUND)
      target_link_libraries(fem_external PRIVATE PkgConfig::QUCS_MUMPS_D PkgConfig::QUCS_MUMPS_Z)
      set(QUCS_MUMPS_FOUND TRUE)
      set(QUCS_MUMPS_FROM "pkg-config mumps-dto, mumps-zto (${QUCS_MUMPS_D_VERSION})")
    endif()
  endif()
  if(NOT QUCS_MUMPS_FOUND)
    find_path(MUMPS_INCLUDE_DIR dmumps_c.h HINTS ${MUMPS_ROOT} ENV MUMPS_ROOT PATH_SUFFIXES include include/mumps)
    # (The sequential ones first: Debian's lib*mumps.so beside them are MPI's.)
    foreach(lib dmumps zmumps mumps_common pord mpiseq)
      find_library(MUMPS_${lib}_LIBRARY NAMES ${lib}_seq ${lib} HINTS ${MUMPS_ROOT} ENV MUMPS_ROOT PATH_SUFFIXES lib lib64)
    endforeach()
    if(MUMPS_INCLUDE_DIR AND MUMPS_dmumps_LIBRARY AND MUMPS_zmumps_LIBRARY AND MUMPS_mumps_common_LIBRARY AND MUMPS_mpiseq_LIBRARY)
      target_include_directories(fem_external SYSTEM PRIVATE ${MUMPS_INCLUDE_DIR})
      target_link_libraries(fem_external PRIVATE ${MUMPS_dmumps_LIBRARY} ${MUMPS_zmumps_LIBRARY} ${MUMPS_mumps_common_LIBRARY})
      if(MUMPS_pord_LIBRARY)
        target_link_libraries(fem_external PRIVATE ${MUMPS_pord_LIBRARY})
      endif()
      target_link_libraries(fem_external PRIVATE ${MUMPS_mpiseq_LIBRARY} ${MUMPS_EXTRA_LIBRARIES})
      set(QUCS_MUMPS_FOUND TRUE)
      set(QUCS_MUMPS_FROM "${MUMPS_dmumps_LIBRARY}")
    endif()
  endif()
endif()
if(QUCS_MUMPS_FOUND)
  target_compile_definitions(fem_external PRIVATE QUCS_MUMPS)
  message(STATUS "Multiphysics sparse solver: MUMPS (${QUCS_MUMPS_FROM})")
else()
  message(STATUS "Multiphysics sparse solver: no MUMPS (QUCS_WITH_MUMPS ${QUCS_WITH_MUMPS})")
  if(QUCS_WITH_MUMPS AND QUCS_SPARSE_SOLVERS_REQUIRED)
    message(FATAL_ERROR "MUMPS not found (MUMPS_ROOT, or pkg-config's mumps-dto and mumps-zto) and QUCS_SPARSE_SOLVERS_REQUIRED is on")
  endif()
endif()
set(QUCS_STRUMPACK_FOUND FALSE)
if(QUCS_WITH_STRUMPACK)
  find_package(STRUMPACK CONFIG QUIET)
  if(STRUMPACK_FOUND AND TARGET STRUMPACK::strumpack)
    target_link_libraries(fem_external PRIVATE STRUMPACK::strumpack)
    target_compile_definitions(fem_external PRIVATE QUCS_STRUMPACK)
    set(QUCS_STRUMPACK_FOUND TRUE)
  endif()
endif()
if(QUCS_STRUMPACK_FOUND)
  message(STATUS "Multiphysics sparse solver: STRUMPACK ${STRUMPACK_VERSION} (${STRUMPACK_DIR})")
else()
  message(STATUS "Multiphysics sparse solver: no STRUMPACK (QUCS_WITH_STRUMPACK ${QUCS_WITH_STRUMPACK})")
  if(QUCS_WITH_STRUMPACK AND QUCS_SPARSE_SOLVERS_REQUIRED)
    message(FATAL_ERROR "STRUMPACK not found (STRUMPACK_DIR) and QUCS_SPARSE_SOLVERS_REQUIRED is on")
  endif()
endif()
