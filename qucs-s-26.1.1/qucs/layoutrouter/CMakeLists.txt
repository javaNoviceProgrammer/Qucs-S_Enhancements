# The layout viewer's metal router (router.h): Qt Core, gdstk to write the
# routes, and the Clipper gdstk carries for the checks on the shapes drawn.
# A library of its own so that its tests build without the application.
# (docs/feature_gaps/2026-11-07-layout-metal-router.md)

set(ROUTER_SRCS
    router_geometry.cpp
    router_tech.cpp
    router_job.cpp
    router_engine.cpp
    router_verify.cpp
    router_gds.cpp
    router_assign.cpp)

add_library(layoutrouter STATIC ${ROUTER_SRCS})
set_target_properties(layoutrouter PROPERTIES POSITION_INDEPENDENT_CODE ON AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
target_include_directories(layoutrouter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# (Clipper's header, which gdstk keeps to itself.)
target_include_directories(layoutrouter SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/gdstk/external)
target_link_libraries(layoutrouter PUBLIC Qt6::Core gdstk)
find_package(Threads REQUIRED)
target_link_libraries(layoutrouter PUBLIC Threads::Threads)
