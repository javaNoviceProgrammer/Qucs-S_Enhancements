# Unit tests for the qucs-s core. Each test is a small QtTest executable that
# links the same objects as the application (qucs-core) and runs headless
# (QT_QPA_PLATFORM=offscreen), so it can be driven by CTest and by CI under
# the sanitizers.

find_package(Qt6 COMPONENTS Test QUIET)
if(NOT Qt6Test_FOUND)
  message(STATUS "Qt6::Test not found - qucs-s unit tests will not be built")
  return()
endif()

set(QUCS_EXAMPLES_DIR "${CMAKE_SOURCE_DIR}/examples")

# qucs_add_test(<name>) : builds tests/<name>.cpp and registers it with CTest.
function(qucs_add_test name)
  add_executable(${name}
    ${name}.cpp
    $<TARGET_OBJECTS:qucs-core>
    ${CMAKE_CURRENT_SOURCE_DIR}/../qucs.qrc)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_compile_definitions(${name} PRIVATE
    QUCS_EXAMPLES_DIR="${QUCS_EXAMPLES_DIR}"
    HAVE_CONFIG_H)
  target_link_libraries(${name} ${QUCS_LINK_LIBRARIES} Qt6::Test)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0"
    TIMEOUT 300)
endfunction()

qucs_add_test(test_schematic_lifetime)
qucs_add_test(test_crash_reports)
qucs_add_test(test_autosave)
qucs_add_test(test_loader)
