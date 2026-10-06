# gdstk 1.0.1, the C++ library that reads and writes GDSII and OASIS
# layouts (README.qucs.md): built statically into Qucs-S for its layout
# viewer (qucs/layoutdoc.h). Only the library: no Python module, no
# examples, no install. Its own CMakeLists.txt asks for CMake 3.26 and
# Qhull; this one for neither (the convex hull is our own).

find_package(ZLIB REQUIRED)

set(GDSTK_SOURCES
    src/cell.cpp src/clipper_tools.cpp src/curve.cpp src/flexpath.cpp src/gdsii.cpp src/label.cpp
    src/layername.cpp src/library.cpp src/oasis.cpp src/polygon.cpp src/property.cpp src/raithdata.cpp
    src/rawcell.cpp src/reference.cpp src/repetition.cpp src/robustpath.cpp src/style.cpp src/utils.cpp
    external/clipper/clipper.cpp)

add_library(gdstk STATIC ${GDSTK_SOURCES})
target_compile_features(gdstk PUBLIC cxx_std_11)
set_target_properties(gdstk PROPERTIES POSITION_INDEPENDENT_CODE ON)
# (SYSTEM: its headers' warnings are not ours either.)
target_include_directories(gdstk SYSTEM
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external)
target_link_libraries(gdstk PUBLIC ZLIB::ZLIB)
if(UNIX)
  target_link_libraries(gdstk PUBLIC m)
endif()
# Its own code, as its authors build it: their warnings are not ours. Its
# GDSII reader reads 8-byte reals 4 bytes into a record, as x86-64 and
# ARM64 do: UBSan's alignment check (CI's sanitizer build) is not for it.
if(MSVC)
  target_compile_options(gdstk PRIVATE /W0 /wd4996)
else()
  target_compile_options(gdstk PRIVATE -w -fno-sanitize=alignment)
endif()
