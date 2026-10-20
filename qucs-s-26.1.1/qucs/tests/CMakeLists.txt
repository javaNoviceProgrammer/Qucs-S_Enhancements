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
  set(QUCS_OWN_LIBRARIES components diagrams dialogs geometry paintings extsimkernels spicecomponents magnetics qt3_compat fem gdstk)
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
  # (The multiphysics engine's headers likewise: a window's test reads them.)
  target_include_directories(qucs-test-core SYSTEM INTERFACE
    $<TARGET_PROPERTY:fem,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:eigen,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:cdt,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(qucs-test-core INTERFACE $<TARGET_PROPERTY:fem,INTERFACE_COMPILE_DEFINITIONS>)
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
    ${CMAKE_CURRENT_SOURCE_DIR}/sanitizer_options.cpp
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

# qucs_add_fem_test(<name>) : a test of the multiphysics engine alone
# (multiphysics/, Qt Core): no window, no core of the application - quick to
# build, under the sanitizers too.
function(qucs_add_fem_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} fem Qt6::Test)
  target_compile_definitions(${name} PRIVATE QUCS_EXAMPLES_DIR="${QUCS_EXAMPLES_DIR}")
  add_test(NAME ${name} COMMAND ${name})
  # (A sanitized Debug build on CI's runners: the semiconductors' solves
  # take minutes there.)
  set_tests_properties(${name} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
    TIMEOUT 900)
endfunction()

qucs_add_fem_test(test_fem_engine)
qucs_add_fem_test(test_fem_sparse)
qucs_add_fem_test(test_fem_physics)
qucs_add_fem_test(test_fem_magnetics)
qucs_add_fem_test(test_fem_layout)
qucs_add_fem_test(test_fem_waves)
# (2056 s on CI's sanitized Debug at 3bf5821, its limit 900 times its scale
# of 3; an SOI wire's mode choices and a thermo-optic loss since - some 280 s
# sanitized here.)
set_tests_properties(test_fem_waves PROPERTIES TIMEOUT 1800)
qucs_add_fem_test(test_fem_semiconductor)
# (864 s and a timeout at 900 on CI's runners, 613 s on a sanitized Debug
# build at home: its contacts' currents, its examples.)
set_tests_properties(test_fem_semiconductor PROPERTIES TIMEOUT 1800)
qucs_add_fem_test(test_fem_rays)
qucs_add_fem_test(test_fem_quantum)
# (697 s on a sanitized Debug build at home, its k·p bands' tests the most:
# Eigen's complex solves unoptimized; each function under 130 s. 1660 s by
# 28dab4c, and past its 5400 s on CI's runners: its functions in two parts
# since - each its own limit, the two at once -, its init() skipping the
# other part's.)
set_tests_properties(test_fem_quantum PROPERTIES TIMEOUT 1800)
set_property(TEST test_fem_quantum APPEND PROPERTY ENVIRONMENT "QUCS_TEST_PART=1")
add_test(NAME test_fem_quantum_2 COMMAND test_fem_quantum)
set_tests_properties(test_fem_quantum_2 PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;QUCS_TEST_PART=2"
  TIMEOUT 1800)
# (The 8x8's and wurtzite's of their own: under the sanitizers each some
# minutes more than the quantum tests' room.)
qucs_add_fem_test(test_fem_exciton)
# (Its coupled excitons' dense solves: some 5 minutes under the sanitizers
# here.)
set_tests_properties(test_fem_exciton PROPERTIES TIMEOUT 1800)
qucs_add_fem_test(test_fem_kp8)
set_tests_properties(test_fem_kp8 PROPERTIES TIMEOUT 1800)
qucs_add_fem_test(test_fem_nitrides)
target_compile_definitions(test_fem_nitrides PRIVATE QUCS_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
set_tests_properties(test_fem_nitrides PROPERTIES TIMEOUT 1800)
qucs_add_fem_test(test_fem_lumerical)
target_compile_definitions(test_fem_lumerical PRIVATE QUCS_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
qucs_add_fem_test(test_fem_materials)
target_compile_definitions(test_fem_materials PRIVATE QUCS_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
qucs_add_fem_test(test_fem_pde)
qucs_add_fem_test(test_fem_anisotropy)
qucs_add_fem_test(test_fem_modulator)
qucs_add_fem_test(test_fem_resonator)
qucs_add_fem_test(test_fem_laser)
qucs_add_fem_test(test_fem_process)
# (It writes a layout with gdstk, a mask's layer to read; it reads the
# flow's insides - Clipper's polygons, the ions' moments.)
target_link_libraries(test_fem_process gdstk)
target_include_directories(test_fem_process SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/gdstk/external)
# (Sanitized here some 400 s - anneals on fine grids, two phase shifters'
# semiconductors -: CI's Debug some 2.5 times that.)
set_tests_properties(test_fem_process PROPERTIES TIMEOUT 1800)
# (Sanitized here some 800 s - the toy laser walked to 1.6 V, its threshold's
# cases, a reverse bias -: CI's Debug 2 to 2.5 times that, near 900 times its
# scale.)
set_tests_properties(test_fem_laser PROPERTIES TIMEOUT 1800)
qucs_add_fem_test(test_fem_optoelectronics)
# (Sanitized: some 500 s here, CI's Debug about 1.45 times that.)
set_tests_properties(test_fem_optoelectronics PROPERTIES TIMEOUT 1800)
qucs_add_fem_test(test_fem_devices)
# (Sanitized: some 17 minutes here, CI's Debug about 1.45 times that.)
set_tests_properties(test_fem_devices PROPERTIES TIMEOUT 3600)
# (Their peak footprints - the laser test's 2.4 GB, the resonator's 2.0, the
# quantum test's first part 1.3, the modulator's 1.2, the optoelectronics'
# 1.1 - two or three at once on CI's macOS runner of three processors and
# 7 GB: swapping, the VCSEL functions past their 300 s on 91450f4, the
# quantum test's split having moved the order the tests run in (the hunt of
# 19 October, C1). One of them at a time there; elsewhere the room is
# enough.)
if(APPLE)
  set_property(TEST test_fem_laser test_fem_resonator test_fem_quantum test_fem_modulator test_fem_optoelectronics
               APPEND PROPERTY RESOURCE_LOCK fem_memory)
endif()
# (It writes a gzipped layout, and an OASIS one with gdstk, to read.)
find_package(ZLIB REQUIRED)
target_link_libraries(test_fem_layout gdstk ZLIB::ZLIB)
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
qucs_add_test(test_multiphysics_doc)
qucs_add_test(test_multiphysics_view)
# (The window's multiphysics end to end - every physics' examples solved:
# five minutes and more under the sanitizers on CI's runners, as the
# engine's tests. There 1131 to 2254 s of its 2700 (900 times the scale of
# 3) from 28dab4c to d3e35fe, its time growing with its functions and
# twice as long on a slow runner: 1200, room for a slow one's.)
set_tests_properties(test_multiphysics_doc PROPERTIES TIMEOUT 1200)
# (a layout KLayout wrote, data/klayout_sample.oas: another program's OASIS)
target_compile_definitions(test_layout_doc PRIVATE QUCS_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
# (Lumerical's files: data/lumerical_sample.py writes them - an MQW project
# of each crystal, a data file.)
target_compile_definitions(test_multiphysics_doc PRIVATE QUCS_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
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
# (macOS: the program links Accelerate weakly - its LU's and complex
# factorizations' symbols macOS 15.5's, the release built for 13.0: linked
# strongly, it would not start on an older macOS; multiphysics/CMakeLists.)
if(APPLE AND QUCS_ACCELERATE_FRAMEWORK)
  add_test(NAME test_macos_accelerate_weak
           COMMAND sh -c "otool -l \"$1\" | grep -A2 LC_LOAD_WEAK_DYLIB | grep -q Accelerate.framework && ! nm -m \"$1\" | grep '(from Accelerate)' | grep -vq 'weak external'"
                   check $<TARGET_FILE:${QUCS_NAME}>)
endif()
# (Its zoomed diagrams drawn at a device pixel ratio of 2, as on a Retina
# screen - QT_SCALE_FACTOR offscreen: the lines in sight taken in the
# widget's pixels, not the device's, passed over only there.)
add_test(NAME test_canvas_drawing_ratio2 COMMAND test_canvas_drawing aZoomedDiagramDrawsEveryLineInSight aPlotOfManyCurvesZoomsSmoothly
         aSlopeZoomedInKeepsItsSlope aCurveIsThinnedToADevicePixel)
set_tests_properties(test_canvas_drawing_ratio2 PROPERTIES
  ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2;ASAN_OPTIONS=detect_leaks=0;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;QUCS_CLAUDE=/nonexistent/claude;QUCS_GH=/nonexistent/gh"
  TIMEOUT 300)
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
target_compile_definitions(test_data_import PRIVATE QUCS_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
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
# And no vendor's project, script or data file (Ansys's examples stay out of
# the repository; ours are the test data's, each its generator's), no
# vendor's copyright line.
set(QUCS_THIRD_PARTY_CHECK "${CMAKE_SOURCE_DIR}/../scripts/ci/check-third-party.py")
if(Python3_Interpreter_FOUND AND EXISTS "${QUCS_THIRD_PARTY_CHECK}")
  add_test(NAME check_third_party COMMAND Python3::Interpreter "${QUCS_THIRD_PARTY_CHECK}")
endif()

# A slow machine's room: each test's TIMEOUT and QtTest's limit on one test
# function (300 s by default) times QUCS_TEST_TIMEOUT_SCALE. CI's sanitized
# Debug job sets it: from October 2026 its runners took twice the time on
# the same image and compiler (test_fem_pde 144 s, then 294), and tests
# under their limits on a quiet runner timed out.
set(QUCS_TEST_TIMEOUT_SCALE 1 CACHE STRING "Each test's TIMEOUT and QtTest's function limit multiplied by this (a whole number)")
if(NOT QUCS_TEST_TIMEOUT_SCALE EQUAL 1)
  math(EXPR _qucs_function_ms "300000 * ${QUCS_TEST_TIMEOUT_SCALE}")
  get_property(_qucs_tests DIRECTORY PROPERTY TESTS)
  foreach(_qucs_test IN LISTS _qucs_tests)
    get_test_property(${_qucs_test} TIMEOUT _qucs_timeout)
    if(_qucs_timeout)
      math(EXPR _qucs_timeout "${_qucs_timeout} * ${QUCS_TEST_TIMEOUT_SCALE}")
      set_tests_properties(${_qucs_test} PROPERTIES TIMEOUT ${_qucs_timeout})
    endif()
    set_property(TEST ${_qucs_test} APPEND PROPERTY ENVIRONMENT "QTEST_FUNCTION_TIMEOUT=${_qucs_function_ms}")
  endforeach()
endif()
# (On macOS the memory-heavy tests above run one at a time, but beside the
# others, and on CI's runner of three processors their solves are some 15
# times as slow as on a workstation's many: the VCSEL example's two
# functions 200 to 300 s there, 14 s here - QtTest's 300 s left them no
# room, and 34e9659's run went red by one (the gain table's ends). Their
# functions 900 s there, three times the default, a hang still ended by the
# test's TIMEOUT of 900 s or more; and appended after the scale's, its value
# when larger.)
if(APPLE)
  math(EXPR _qucs_memory_ms "300000 * ${QUCS_TEST_TIMEOUT_SCALE}")
  if(_qucs_memory_ms LESS 900000)
    set(_qucs_memory_ms 900000)
  endif()
  set_property(TEST test_fem_laser test_fem_resonator test_fem_quantum test_fem_modulator test_fem_optoelectronics
               APPEND PROPERTY ENVIRONMENT "QTEST_FUNCTION_TIMEOUT=${_qucs_memory_ms}")
endif()
# (The toy laser's threshold cases - three walks to 1.4 V and more, one of
# them failing on purpose, halved to 10 µV and tried again from rest - some
# 300 s sanitized here; CI's sanitized Debug runner twice that or so, and a
# slow one past QtTest's 900 s there (d3e35fe's run, its tests 188 minutes
# where 953c8c9's took 90). The laser test's functions twice the scale's,
# within its TIMEOUT; on macOS 900 s at least, as above.)
math(EXPR _qucs_laser_ms "600000 * ${QUCS_TEST_TIMEOUT_SCALE}")
if(APPLE AND _qucs_laser_ms LESS 900000)
  set(_qucs_laser_ms 900000)
endif()
set_property(TEST test_fem_laser APPEND PROPERTY ENVIRONMENT "QTEST_FUNCTION_TIMEOUT=${_qucs_laser_ms}")
