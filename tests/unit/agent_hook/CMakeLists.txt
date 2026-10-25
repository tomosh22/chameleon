# WP-10: the hook installer.
#
#   unit.agent_hook         instruction-length decoder and strategy planning (portable: byte vectors),
#                           the installer on hand-assembled and compiled functions, thread
#                           suspension and convergence, and InstallHooks through the agent's
#                           dispatcher (Windows)
#   unit.agent_hook_stress  60 s with 16 threads spinning through hooked functions while the main thread
#                           installs, re-points and reverts them (Windows); its own executable so that a
#                           failure of the quick tests never costs a minute, and so that its TIMEOUT
#                           (and with it the hang watchdog, TIMEOUT minus 30 s) fits the 60 s run
#
# The stress test appends its WP10-STRESS lines to ${CMAKE_BINARY_DIR}/test_work/agent_hook/evidence.txt,
# which the CI workflow prints.

cml_add_unit_test(agent_hook
    SOURCES test_length_decoder.cpp test_plan.cpp
    LIBS cml_agent_core)

if(WIN32)
    # agent/src/operator_new.cpp is what cml_agent.dll links: with it in the test executables every
    # `new` goes through the agent's heap, so heap::set_alloc_forbidden() is really exercised (see
    # alloc_guard.h).
    set(CML_AGENT_OPERATOR_NEW "${PROJECT_SOURCE_DIR}/agent/src/operator_new.cpp")
    target_sources(cml_test_agent_hook PRIVATE test_installer.cpp test_hardening.cpp test_hang_watch.cpp test_dispatch.cpp
                                               ${CML_AGENT_OPERATOR_NEW})
    target_include_directories(cml_test_agent_hook PRIVATE "${PROJECT_SOURCE_DIR}/agent/include")
    # Test functions are located by address: no incremental-link thunks, no identical-code folding.
    target_link_options(cml_test_agent_hook PRIVATE /INCREMENTAL:NO /OPT:NOICF)
    set_tests_properties(unit.agent_hook PROPERTIES TIMEOUT 300)
    # The out-of-process hang watchdog (test_hang_watch.cpp tests it; doctest_main.cpp starts it for the
    # executables that stop the world, because the in-process watchdog is stopped with everything else).
    add_dependencies(cml_test_agent_hook cml_test_hang_watch)
    target_compile_definitions(cml_test_agent_hook PRIVATE "CML_HANG_WATCH_PATH=\"$<TARGET_FILE:cml_test_hang_watch>\"")
    set_property(TEST unit.agent_hook APPEND PROPERTY ENVIRONMENT "CML_TEST_HANG_WATCH_EXE=$<TARGET_FILE:cml_test_hang_watch>")

    cml_add_unit_test(agent_hook_stress SOURCES test_stress.cpp ${CML_AGENT_OPERATOR_NEW} LIBS cml_agent_core)
    target_link_options(cml_test_agent_hook_stress PRIVATE /INCREMENTAL:NO /OPT:NOICF)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test_work/agent_hook")
    target_compile_definitions(cml_test_agent_hook_stress PRIVATE
        "CML_TEST_WORK_DIR=\"${CMAKE_BINARY_DIR}/test_work/agent_hook\"")
    set_tests_properties(unit.agent_hook_stress PROPERTIES TIMEOUT 400)
    add_dependencies(cml_test_agent_hook_stress cml_test_hang_watch)
    set_property(TEST unit.agent_hook_stress APPEND PROPERTY ENVIRONMENT "CML_TEST_HANG_WATCH_EXE=$<TARGET_FILE:cml_test_hang_watch>")
endif()

# The decoder against every function of a real module (CI evidence; see docs/wp/WP-10.md). The ctest
# entry runs it on fixture `basic` (MSVC-style builds only: it needs the PDB) and checks it found functions.
add_executable(cml_lde_probe lde_probe.cpp)
target_link_libraries(cml_lde_probe PRIVATE cml_pdb cml_pe cml_agent_core)
cml_apply_warnings(cml_lde_probe)
cml_target_core_style(cml_lde_probe)
set_target_properties(cml_lde_probe PROPERTIES FOLDER "tools")
if(MSVC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/basic/CMakeLists.txt")
    add_dependencies(cml_lde_probe fixture_basic)
    add_test(NAME tool.lde_probe_basic COMMAND cml_lde_probe $<TARGET_FILE:fixture_basic> --quiet)
    set_tests_properties(tool.lde_probe_basic PROPERTIES
        PASS_REGULAR_EXPRESSION "LDE-SUMMARY functions=[1-9][0-9]* decoded=[1-9][0-9]*"
        TIMEOUT 300)
endif()
