# cmltest (WP-13): the scenario harness.
#
#   cmltest_core   portable pieces: TOML-subset reader, scenario model, regex engine, unified-diff
#                  applier, report/JUnit writer. Built and unit-tested on every host.
#   cmltest        Windows only: process supervision (job objects, minidumps), fixture builder (CMake
#                  with Ninja or Visual Studio generators), control-pipe client use, the runner.
#
# Unit tests (ctest names unit.harness and unit.control) live in tests/harness/tests; they are
# registered through cml_add_unit_test() from tests/unit/CMakeLists.txt, which is processed first.

# The unit-test helper takes the doctest main from this variable (set in tests/unit's own scope).
set(CML_UNIT_MAIN "${PROJECT_SOURCE_DIR}/tests/unit/doctest_main.cpp")

add_library(cmltest_core STATIC
    src/toml.cpp
    src/rx.cpp
    src/unified_diff.cpp
    src/scenario.cpp
    src/report.cpp)
target_include_directories(cmltest_core PUBLIC src)
target_link_libraries(cmltest_core PUBLIC cml_ipc cml_util)
cml_apply_warnings(cmltest_core)
cml_target_exceptions_allowed(cmltest_core)
set_target_properties(cmltest_core PROPERTIES FOLDER "tests/harness")

if(WIN32)
    add_executable(cmltest
        src/main.cpp
        src/supervisor.cpp
        src/fixture_builder.cpp
        src/runner.cpp)
    target_link_libraries(cmltest PRIVATE cmltest_core cml_ipc cml_proc dbghelp)
    cml_apply_warnings(cmltest)
    cml_target_exceptions_allowed(cmltest)
    set_target_properties(cmltest PROPERTIES FOLDER "tests/harness")
    # The harness starts these by path; they must be built before it runs. The fixture itself is
    # built by cmltest from a scratch copy, so no fixture target is needed here.
    add_dependencies(cmltest cml_broker cml_agent)

    # cmltest --help / bad usage: exit codes only (the real scenario run is the `cmltest` CI step).
    add_test(NAME harness.cmltest_help
        COMMAND ${CMAKE_COMMAND}
            "-DCML_EXE=$<TARGET_FILE:cmltest>"
            "-DCML_ARGS=--help"
            "-DCML_EXPECT_RC=0"
            -P "${PROJECT_SOURCE_DIR}/tests/cmake/run_and_check.cmake")
    add_test(NAME harness.cmltest_usage_error
        COMMAND ${CMAKE_COMMAND}
            "-DCML_EXE=$<TARGET_FILE:cmltest>"
            "-DCML_ARGS=--generator ninja"
            "-DCML_EXPECT_RC=2"
            -P "${PROJECT_SOURCE_DIR}/tests/cmake/run_and_check.cmake")
    add_test(NAME harness.cmltest_bad_scenario_fails
        COMMAND ${CMAKE_COMMAND}
            "-DCML_EXE=$<TARGET_FILE:cmltest>"
            "-DCML_ARGS=--scenario ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/invalid_scenario.toml"
            "-DCML_EXPECT_RC=1"
            -P "${PROJECT_SOURCE_DIR}/tests/cmake/run_and_check.cmake")
    set_tests_properties(harness.cmltest_help harness.cmltest_usage_error harness.cmltest_bad_scenario_fails PROPERTIES TIMEOUT 60)
endif()

cml_add_unit_test(harness
    SOURCES
        tests/test_toml_scenario.cpp
        tests/test_rx.cpp
        tests/test_unified_diff.cpp
        tests/test_report.cpp
    LIBS cmltest_core)
target_compile_definitions(cml_test_harness PRIVATE
    "CML_HARNESS_SCENARIOS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/scenarios\""
    "CML_FIXTURE_BASIC_EDIT=\"${PROJECT_SOURCE_DIR}/tests/fixtures/basic/edits/value_2.edit\""
    "CML_FIXTURE_BASIC_DIR=\"${PROJECT_SOURCE_DIR}/tests/fixtures/basic\"")

cml_add_unit_test(control
    SOURCES
        tests/test_json.cpp
        tests/test_control.cpp
    LIBS cml_core cml_ipc)
if(WIN32)
    target_sources(cml_test_control PRIVATE tests/test_control_pipe.cpp)
    set_tests_properties(unit.control PROPERTIES TIMEOUT 300)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(cml_test_control PRIVATE Threads::Threads)
endif()
