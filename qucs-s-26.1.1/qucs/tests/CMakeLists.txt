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
    # UBSan only prints by default; make a report fail the test.
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
    TIMEOUT 300)
endfunction()

qucs_add_test(test_schematic_lifetime)
qucs_add_test(test_crash_reports)
qucs_add_test(test_autosave)
qucs_add_test(test_loader)
qucs_add_test(test_simout)
qucs_add_test(test_build_all_va)
qucs_add_test(test_drop_open)
qucs_add_test(test_project_scratch)
qucs_add_test(test_keyboard_move)
qucs_add_test(test_diagram_legend)
qucs_add_test(test_simulation_console)
qucs_add_test(test_process_console)
qucs_add_test(test_panes)
qucs_add_test(test_shell_environment)
qucs_add_test(test_app_theme)
qucs_add_test(test_net_highlight)
qucs_add_test(test_erc)
qucs_add_test(test_spice_options)
qucs_add_test(test_netlist_audit)
qucs_add_test(test_netlist_fixes)
qucs_add_test(test_symbol_image)
qucs_add_test(test_symbol_pins)
qucs_add_test(test_symbol_tools)

# Not a test: the driver scripts/ci/fuzz-simout.py runs over damaged
# simulator output (see simout_harness.cpp).
add_executable(simout_harness
  simout_harness.cpp
  $<TARGET_OBJECTS:qucs-core>
  ${CMAKE_CURRENT_SOURCE_DIR}/../qucs.qrc)
target_include_directories(simout_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(simout_harness PRIVATE HAVE_CONFIG_H)
target_link_libraries(simout_harness ${QUCS_LINK_LIBRARIES})
qucs_add_test(test_simulator_modes)
qucs_add_test(test_component_search)
qucs_add_test(test_bias_labels)
qucs_add_test(test_ink)
qucs_add_test(test_subcircuit_dialog)
qucs_add_test(test_value_reading)
