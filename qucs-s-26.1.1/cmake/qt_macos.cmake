# Qt's own macOS builds (the Qt installer's, aqtinstall's) on current Xcode:
# three things Homebrew's Qt does not need. Each is found by compiling a line
# the way the project compiles against Qt, and fixed only where found, so
# nothing changes where it is not needed: 1 on Qt6::Platform, which carries
# the option, and the subdirectories' find_package(Qt6) calls reuse that
# target; 2 and 3 as C++ options of the whole tree, the way Qt's headers reach
# it (some libraries take them from include_directories alone: qucs/components
# and qucs/qt3_compat link no Qt target). Included by the top-level
# CMakeLists.txt right after its find_package(Qt6), ahead of every
# add_subdirectory.
#
# 1. A space in Qt's path. Qt 6.12's Qt6::Platform passes
#    "SHELL:-idirafter <Qt>/include", which CMake splits at the space (as in
#    "~/Qt Kits/6.12.0/macos"), and every file then fails on a directory that
#    is not there. Rewritten as one argument.
# 2. Qt 6.10 on current Xcode. <QtCore/qyieldcpu.h> calls __yield() whenever
#    __has_builtin(__yield) allows, without <arm_acle.h>, which declares it,
#    and Apple clang from Xcode with the macOS 27 SDK makes that an error in
#    every file that includes Qt (Qt 6.12 asks for __builtin_arm_yield
#    first). qt_arm_acle.h, beside this file, is included ahead of every C++
#    file; it brings <arm_acle.h> in on ARM only, which leaves a universal
#    build's Intel half alone.
# 3. Qt 6.12's framework headers. Each module's include directory is
#    <Qt>/include/<Module>, of one-line forwarders - its qwidget.h is
#    "#include_next <QtWidgets/qwidget.h>" - and the frameworks come in as a
#    single -F<Qt>/lib, after the first module's directory. A later module's
#    forwarder then finds nothing after itself, and an unqualified include
#    such as "qwidget.h" (qt3_compat and other older code) fails. Homebrew's
#    forwarders fall back when nothing follows; Qt's own do not. The
#    frameworks are searched once more as a system directory, which comes
#    after every -I. Checked with 2's fix in: its check includes Qt's
#    headers, which fail without it where 2 applies.
#
# 2 and 3 come of qucs/ and the tools naming Qt's include directories
# themselves (include_directories(${Qt6Core_INCLUDE_DIRS} ...)): Qt's headers
# come in by plain -I and -F, in the project's module order, so they are not
# system headers - clang would let 2 pass in one. The checks compile the same
# way: Qt's include directories given plainly and no Qt target, whose
# directories a check project (on CMake's newest policies, where this project
# asks for 3.10's) would make system ones.

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

# Whether <source> compiles with <includes> as plain include directories and
# any further arguments as extra compiler flags (each one argument), as C++17
# at least (Qt 6's headers need it). Into the cached <var>.
function(qucs_qt_compiles var source includes)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_INCLUDES ${includes})
  if(ARGN)
    # On an imported target, which keeps each flag one argument: a check's
    # own flags (CMAKE_REQUIRED_DEFINITIONS) are split at spaces, as in a path.
    if(NOT TARGET qucs_qt_check_flags)
      add_library(qucs_qt_check_flags INTERFACE IMPORTED)
    endif()
    set_property(TARGET qucs_qt_check_flags PROPERTY INTERFACE_COMPILE_OPTIONS ${ARGN})
    set(CMAKE_REQUIRED_LIBRARIES qucs_qt_check_flags)
  endif()
  set(CMAKE_REQUIRED_QUIET ON)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  if(NOT CMAKE_CXX_STANDARD OR CMAKE_CXX_STANDARD LESS 17)
    set(CMAKE_CXX_STANDARD 17)
  endif()
  check_cxx_source_compiles("${source}" ${var})
  cmake_pop_check_state()
endfunction()

# 1. A space in Qt's path: first, as the checks below compile against Qt.
get_target_property(_qucs_options Qt6::Platform INTERFACE_COMPILE_OPTIONS)
if(_qucs_options MATCHES "SHELL:-idirafter ([^;]* [^;]*)")
  string(REPLACE "SHELL:-idirafter ${CMAKE_MATCH_1}" "-idirafter${CMAKE_MATCH_1}"
         _qucs_options "${_qucs_options}")
  set_property(TARGET Qt6::Platform PROPERTY INTERFACE_COMPILE_OPTIONS "${_qucs_options}")
  message(STATUS "Qt: its -idirafter passed as one argument, for the space in its path")
endif()

# 2. Qt 6.10 on current Xcode.
set(_qucs_flags "")
set(_qucs_source "#if __has_include(<QtCore/qyieldcpu.h>)\n#include <QtCore/qyieldcpu.h>\n#endif\nint main() { return 0; }\n")
qucs_qt_compiles(QUCS_QT_YIELDCPU "${_qucs_source}" "${Qt6Core_INCLUDE_DIRS}")
if(NOT QUCS_QT_YIELDCPU)
  set(_qucs_acle "${CMAKE_CURRENT_LIST_DIR}/qt_arm_acle.h")
  qucs_qt_compiles(QUCS_QT_YIELDCPU_ARM_ACLE "${_qucs_source}" "${Qt6Core_INCLUDE_DIRS}"
                   "-include${_qucs_acle}")
  if(QUCS_QT_YIELDCPU_ARM_ACLE)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-include${_qucs_acle}>")
    list(APPEND _qucs_flags "-include${_qucs_acle}")
    message(STATUS "Qt: <arm_acle.h> included ahead of its headers, for its qyieldcpu.h on this compiler")
  endif()
endif()

# 3. Qt 6.12's framework headers.
get_target_property(_qucs_includes Qt6::Core INTERFACE_INCLUDE_DIRECTORIES)
set(_qucs_frameworks "")
foreach(_qucs_dir IN LISTS _qucs_includes)
  if(_qucs_dir MATCHES "^(.*)/QtCore\\.framework/?$")
    set(_qucs_frameworks "${CMAKE_MATCH_1}")
  endif()
endforeach()
if(_qucs_frameworks)
  set(_qucs_source "#include \"qwidget.h\"\nint main() { return 0; }\n")
  set(_qucs_dirs ${Qt6Core_INCLUDE_DIRS} ${Qt6Widgets_INCLUDE_DIRS})
  qucs_qt_compiles(QUCS_QT_UNQUALIFIED_INCLUDES "${_qucs_source}" "${_qucs_dirs}" ${_qucs_flags})
  if(NOT QUCS_QT_UNQUALIFIED_INCLUDES)
    qucs_qt_compiles(QUCS_QT_UNQUALIFIED_INCLUDES_FRAMEWORKS_LAST "${_qucs_source}" "${_qucs_dirs}"
                     ${_qucs_flags} "-iframework${_qucs_frameworks}")
    if(QUCS_QT_UNQUALIFIED_INCLUDES_FRAMEWORKS_LAST)
      add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-iframework${_qucs_frameworks}>")
      message(STATUS "Qt: its frameworks searched again after the include directories, for its forwarding headers")
    endif()
  endif()
endif()
