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

# Each test links the core as the application does: its objects and the
# static libraries, a copy in every executable. With QUCS_SHARED_TEST_CORE
# they are linked once instead, into a shared library every test uses - for
# a build where the copies cost: a Debug build under the sanitizers on Linux
# wrote the core's debug info into each of some 95 tests, 89 GB (CI turns
# it on). The tests are compiled as before; only their link differs.
option(QUCS_SHARED_TEST_CORE "Link the unit tests against one shared library of the core, not each a copy of it" OFF)
if(QUCS_SHARED_TEST_CORE)
  if(CMAKE_VERSION VERSION_LESS 3.24)
    message(FATAL_ERROR "QUCS_SHARED_TEST_CORE needs CMake 3.24 or later (LINK_LIBRARY:WHOLE_ARCHIVE)")
  endif()
  # The project's static libraries whole (a test calls what the core does
  # not), the others - Qt, zlib through gdstk, util - as they are.
  set(QUCS_OWN_LIBRARIES components diagrams dialogs geometry paintings extsimkernels spicecomponents magnetics qt3_compat gdstk)
  set(QUCS_OTHER_LIBRARIES ${QUCS_LINK_LIBRARIES})
  list(REMOVE_ITEM QUCS_OTHER_LIBRARIES ${QUCS_OWN_LIBRARIES})
  string(JOIN "," QUCS_OWN_LIBRARIES_LIST ${QUCS_OWN_LIBRARIES})
  add_library(qucs-test-core SHARED $<TARGET_OBJECTS:qucs-core>)
  set_target_properties(qucs-test-core PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
  target_link_libraries(qucs-test-core
    PUBLIC ${QUCS_OTHER_LIBRARIES}
    PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${QUCS_OWN_LIBRARIES_LIST}>")
  # (gdstk's headers and zlib, as linking it gave them: test_layout_doc
  # reads the headers and writes a .gds.gz itself.)
  find_package(ZLIB REQUIRED)
  target_link_libraries(qucs-test-core PUBLIC ZLIB::ZLIB)
  target_include_directories(qucs-test-core SYSTEM INTERFACE $<TARGET_PROPERTY:gdstk,INTERFACE_INCLUDE_DIRECTORIES>)
  set(QUCS_TEST_CORE_OBJECTS "")
  set(QUCS_TEST_LINK_LIBRARIES qucs-test-core)
else()
  set(QUCS_TEST_CORE_OBJECTS $<TARGET_OBJECTS:qucs-core>)
  set(QUCS_TEST_LINK_LIBRARIES ${QUCS_LINK_LIBRARIES})
endif()

# qucs_add_test(<name>) : builds tests/<name>.cpp and registers it with CTest.
function(qucs_add_test name)
  add_executable(${name}
    ${name}.cpp
    ${QUCS_TEST_CORE_OBJECTS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../qucs.qrc)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
  target_compile_definitions(${name} PRIVATE
    QUCS_EXAMPLES_DIR="${QUCS_EXAMPLES_DIR}"
    HAVE_CONFIG_H)
  target_link_libraries(${name} ${QUCS_TEST_LINK_LIBRARIES} Qt6::Test)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES
    # UBSan only prints by default; make a report fail the test.
    # QUCS_CLAUDE: no test reaches a claude program installed on the machine
    # QUCS_GH: nor GitHub, through a gh the Claude Code dock's git bar would ask
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;QUCS_CLAUDE=/nonexistent/claude;QUCS_GH=/nonexistent/gh"
    TIMEOUT 300)
endfunction()

qucs_add_test(test_schematic_lifetime)
qucs_add_test(test_crash_reports)
qucs_add_test(test_build_commit)
add_dependencies(test_build_commit qucs-commit)   # (qucs_commit.h)
qucs_add_test(test_autosave)
qucs_add_test(test_loader)
qucs_add_test(test_simout)
qucs_add_test(test_build_all_va)
qucs_add_test(test_library_verilog_a)
qucs_add_test(test_project_libraries)
qucs_add_test(test_library_paths)
qucs_add_test(test_library_export)
# (the converter's Qucs library, when it is built here: its target comes later)
if(EXISTS ${CMAKE_SOURCE_DIR}/qucsator_rf/CMakeLists.txt)
  target_compile_definitions(test_library_export PRIVATE QUCSCONV="$<TARGET_FILE:qucsconv_rf>")
  add_dependencies(test_library_export qucsconv_rf)
endif()
qucs_add_test(test_text_doc_settings)
qucs_add_test(test_mouse_wheel)
qucs_add_test(test_text_editing)
qucs_add_test(test_settings_io)
qucs_add_test(test_drop_open)
qucs_add_test(test_project_scratch)
qucs_add_test(test_content_categories)
qucs_add_test(test_syntax_highlighting)
qucs_add_test(test_tuner_dock)
qucs_add_test(test_markdown_doc)
qucs_add_test(test_spreadsheet)
qucs_add_test(test_sheet_doc)
qucs_add_test(test_zip_doc)
qucs_add_test(test_layout_doc)
# (a layout KLayout wrote, data/klayout_sample.oas: another program's OASIS)
target_compile_definitions(test_layout_doc PRIVATE QUCS_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
qucs_add_test(test_keyboard_move)
qucs_add_test(test_diagram_legend)
qucs_add_test(test_diagram_theme)
qucs_add_test(test_view_all)
qucs_add_test(test_simulation_console)
qucs_add_test(test_process_console)
qucs_add_test(test_panes)
qucs_add_test(test_binary_dataset)
qucs_add_test(test_shell_environment)
qucs_add_test(test_app_theme)
qucs_add_test(test_status_bar)
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
  ${QUCS_TEST_CORE_OBJECTS}
  ${CMAKE_CURRENT_SOURCE_DIR}/../qucs.qrc)
target_include_directories(simout_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(simout_harness PRIVATE HAVE_CONFIG_H)
target_link_libraries(simout_harness ${QUCS_TEST_LINK_LIBRARIES})
qucs_add_test(test_simulator_modes)
qucs_add_test(test_component_search)
qucs_add_test(test_bias_labels)
qucs_add_test(test_ink)
qucs_add_test(test_subcircuit_dialog)
qucs_add_test(test_value_reading)
qucs_add_test(test_grid_setting)
qucs_add_test(test_toolbar_lock)
qucs_add_test(test_workspace_session)
qucs_add_test(test_marker_colors)
qucs_add_test(test_marker_notation)
qucs_add_test(test_graph_style)
qucs_add_test(test_system_open)
qucs_add_test(test_operating_point)
qucs_add_test(test_workspace_projects)
qucs_add_test(test_project_folders)
qucs_add_test(test_long_file_names)
qucs_add_test(test_claude_git)
qucs_add_test(test_git_integration)
qucs_add_test(test_verilog_a_components)
qucs_add_test(test_optimization)
qucs_add_test(test_ngopt)
qucs_add_test(test_ngstats)
qucs_add_test(test_ngsweep)
qucs_add_test(test_graph_autocolor)
qucs_add_test(test_gui_monkey)
qucs_add_test(test_stress_findings)
qucs_add_test(test_large_schematics)
qucs_add_test(test_canvas_drawing)
qucs_add_test(test_claude_code)
qucs_add_test(test_qucs_control)
# The Tools menu's programs as Claude's tools (their --json): built first
# (the top CMakeLists.txt, once they are known).
qucs_add_test(test_wishlist_tools)
qucs_add_test(test_file_browser)
qucs_add_test(test_file_browser_drop)
qucs_add_test(test_file_browser_tabs)
qucs_add_test(test_new_paintings)
qucs_add_test(test_dataset)
qucs_add_test(test_data_import)
qucs_add_test(test_data_export)
if(TARGET Qt6::Pdf AND QUCS_PDF_VIEWER)
  qucs_add_test(test_pdf_viewer)
endif()
qucs_add_test(test_image_doc)
qucs_add_test(test_xml_doc)
qucs_add_test(test_histogram_diagram)
qucs_add_test(test_stacked_diagram)
qucs_add_test(test_diagram_tools)
qucs_add_test(test_prbs_eye)
qucs_add_test(test_scale_and_memory)
qucs_add_test(test_graphics_export)
qucs_add_test(test_osdi_selection)
qucs_add_test(test_number_format)
qucs_add_test(test_component_symbols)
qucs_add_test(test_python_doc)
qucs_add_test(test_python_tools)
# (qucs.simulate() runs the program itself; the sample schematic of python/.)
target_compile_definitions(test_python_tools PRIVATE QUCS_BINARY="$<TARGET_FILE:${QUCS_NAME}>"
  QUCS_PYTHON_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../python")
add_dependencies(test_python_tools ${QUCS_NAME})
qucs_add_test(test_python_ide)
qucs_add_test(test_python_extras)
target_compile_definitions(test_system_open PRIVATE QUCS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(APPLE)
  target_compile_definitions(test_system_open PRIVATE
    QUCS_BUNDLE_INFO_PLIST="$<TARGET_BUNDLE_CONTENT_DIR:${QUCS_NAME}>/Info.plist")
endif()
qucs_add_test(test_quiet_start)
qucs_add_test(test_mcp_server)
# (Its last case runs the program itself: qucs-s --mcp-server.)
target_compile_definitions(test_mcp_server PRIVATE QUCS_BINARY="$<TARGET_FILE:${QUCS_NAME}>")
add_dependencies(test_mcp_server ${QUCS_NAME})

# Every argument a Claude tool reads is in its schema, and the other way
# round: CI's first step, here too (a tool's dispatch changed into a block
# once, and only CI saw that the check no longer found its handler).
set(QUCS_TOOL_ARGUMENTS_CHECK "${CMAKE_SOURCE_DIR}/../scripts/ci/check-tool-arguments.py")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND EXISTS "${QUCS_TOOL_ARGUMENTS_CHECK}")
  add_test(NAME check_tool_arguments COMMAND Python3::Interpreter "${QUCS_TOOL_ARGUMENTS_CHECK}")
endif()
# And no list of one list written as QJsonArray{QJsonArray{...}}: a copy to
# Apple's newer clang (CI's), a nesting to the others.
set(QUCS_JSON_NESTING_CHECK "${CMAKE_SOURCE_DIR}/../scripts/ci/check-json-nesting.py")
if(Python3_Interpreter_FOUND AND EXISTS "${QUCS_JSON_NESTING_CHECK}")
  add_test(NAME check_json_nesting COMMAND Python3::Interpreter "${QUCS_JSON_NESTING_CHECK}")
endif()
