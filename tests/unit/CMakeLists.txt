# Unit tests: one doctest executable per library (cml_test_<name>), each registered with ctest as
# unit.<name>. Tests may use exceptions (doctest needs them); the code under test does not.

set(CML_UNIT_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/doctest_main.cpp")

# The hang watchdog of doctest_main.cpp is off unless CML_TEST_CASE_TIMEOUT_S is set. Every unit
# test gets it set to its ctest TIMEOUT minus 30 s (ctest's default timeout is 1500 s), so that the
# watchdog speaks, with a minidump, before ctest kills the process silently. TIMEOUT is usually set
# after cml_add_unit_test() returns, so this runs at the end of the calling directory.
function(_cml_unit_test_watchdog test)
    get_test_property(${test} TIMEOUT _timeout)
    if(NOT _timeout OR _timeout STREQUAL "NOTFOUND")
        set(_timeout 1500)
    endif()
    math(EXPR _budget "${_timeout} - 30")
    set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "CML_TEST_CASE_TIMEOUT_S=${_budget}")
endfunction()

function(cml_add_unit_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(cml_test_${name} ${CML_UNIT_MAIN} ${T_SOURCES})
    target_link_libraries(cml_test_${name} PRIVATE cml_third_party_doctest ${T_LIBS})
    cml_apply_warnings(cml_test_${name})
    cml_target_exceptions_allowed(cml_test_${name})
    set_target_properties(cml_test_${name} PROPERTIES FOLDER "tests")
    add_test(NAME unit.${name} COMMAND cml_test_${name})
    # (EVAL: the arguments of a deferred call are otherwise evaluated when it runs, not now.)
    cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY [[${CMAKE_CURRENT_SOURCE_DIR}]] CALL _cml_unit_test_watchdog [[unit.${name}]])")
endfunction()

cml_add_unit_test(util
    SOURCES util/test_diag.cpp util/test_result.cpp util/test_log.cpp util/test_hash.cpp util/test_path.cpp
    LIBS cml_util)
if(NOT MSVC)
    find_package(Threads REQUIRED)
    target_link_libraries(cml_test_util PRIVATE Threads::Threads)
endif()
add_subdirectory(coff)  # WP-02: unit.coff
add_subdirectory(pe)    # WP-02: unit.pe
cml_add_unit_test(pdb
    SOURCES
        lib/test_pdb.cpp
        pdb/test_path.cpp
        pdb/test_synthetic.cpp
        pdb/test_robust.cpp
        pdb/test_pe_lookup.cpp
        pdb/test_fixture.cpp
    LIBS cml_pdb)
# WP-04: pdb/test_fixture.cpp reads the PDB of fixture `basic` (built by this CI cell's compiler).
# Local overrides (portable syntax-check builds have no fixture binaries): CML_TEST_FIXTURE_BASIC_PDB,
# CML_TEST_FIXTURE_BASIC_EXE, CML_TEST_FIXTURE_BASIC_SRC.
if(CML_TEST_FIXTURE_BASIC_PDB AND CML_TEST_FIXTURE_BASIC_EXE AND CML_TEST_FIXTURE_BASIC_SRC)
    target_compile_definitions(cml_test_pdb PRIVATE
        "CML_FIXTURE_BASIC_PDB=\"${CML_TEST_FIXTURE_BASIC_PDB}\""
        "CML_FIXTURE_BASIC_EXE=\"${CML_TEST_FIXTURE_BASIC_EXE}\""
        "CML_FIXTURE_BASIC_SRC=\"${CML_TEST_FIXTURE_BASIC_SRC}\"")
elseif(MSVC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/basic/CMakeLists.txt")
    target_compile_definitions(cml_test_pdb PRIVATE
        "CML_FIXTURE_BASIC_PDB=\"$<TARGET_PDB_FILE:fixture_basic>\""
        "CML_FIXTURE_BASIC_EXE=\"$<TARGET_FILE:fixture_basic>\""
        "CML_FIXTURE_BASIC_SRC=\"${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/basic\"")
    add_dependencies(cml_test_pdb fixture_basic)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_definitions(cml_test_pdb PRIVATE CML_TEST_COMPILER_IS_CL=1)  # cl.exe, not clang-cl
endif()
cml_add_unit_test(ipc SOURCES lib/test_ipc.cpp lib/test_ipc_hooks.cpp lib/test_ipc_loader.cpp LIBS cml_ipc)
cml_add_unit_test(proc SOURCES lib/test_proc.cpp lib/test_proc_address_space.cpp LIBS cml_proc)
if(WIN32)
    # WP-11: named-pipe transport tests (real pipes, in-process client and server threads).
    target_sources(cml_test_ipc PRIVATE lib/test_ipc_pipe.cpp)
    set_tests_properties(unit.ipc unit.proc PROPERTIES TIMEOUT 120)
endif()
add_subdirectory(toolchain)  # WP-05: unit.toolchain
cml_add_unit_test(watch SOURCES lib/test_watch.cpp LIBS cml_watch)
# WP-11: agent pieces (SyncPoint queue, module diff, heap) link the agent core library directly; the
# end-to-end tests load cml_agent.dll through Chameleon_API.h like an application would and talk to
# an in-process broker (cml_core's ConnectionServer).
cml_add_unit_test(agent
    SOURCES agent/test_sync_queue.cpp agent/test_module_diff.cpp
    LIBS cml_agent_core)
if(WIN32)
    # WP-10: out-of-process hang watchdog (the in-process one is stopped together with every other thread
    # when the hook installer stops the world). Started by doctest_main.cpp when CML_TEST_HANG_WATCH_EXE is set.
    add_executable(cml_test_hang_watch hang_watch/hang_watch.cpp)
    target_link_libraries(cml_test_hang_watch PRIVATE psapi)
    cml_apply_warnings(cml_test_hang_watch)
    set_target_properties(cml_test_hang_watch PROPERTIES FOLDER "tests")
endif()
add_subdirectory(agent_hook)  # WP-10: unit.agent_hook, unit.agent_hook_stress
cml_add_unit_test(broker_core SOURCES broker/test_core.cpp broker/test_session.cpp LIBS cml_core)
add_subdirectory(bind)  # WP-07: unit.bind
if(NOT MSVC)
    # Portable pieces are tested on non-Windows hosts too (CML_ALLOW_NON_WINDOWS).
    target_link_libraries(cml_test_agent PRIVATE Threads::Threads)
    target_link_libraries(cml_test_broker_core PRIVATE Threads::Threads)
endif()
if(WIN32)
    target_sources(cml_test_agent PRIVATE agent/test_heap.cpp agent/test_agent_log.cpp agent/test_agent_table.cpp agent/test_agent.cpp)
    target_link_libraries(cml_test_agent PRIVATE cml_core)
    target_include_directories(cml_test_agent PRIVATE "${PROJECT_SOURCE_DIR}/agent/include")
    add_dependencies(cml_test_agent cml_agent cml_broker cml_test_dummy_module cml_test_fake_agent_abi cml_test_fake_broker)
    target_compile_definitions(cml_test_agent PRIVATE "CML_TEST_FAKE_BROKER_EXE=\"$<TARGET_FILE:cml_test_fake_broker>\"")
    # The real broker, for the test that lets the agent auto-start one (and kills it).
    target_compile_definitions(cml_test_agent PRIVATE "CML_TEST_BROKER_EXE=\"$<TARGET_FILE:cml_broker>\"")
    target_sources(cml_test_broker_core PRIVATE broker/test_connection_server.cpp)
    set_tests_properties(unit.agent unit.broker_core PROPERTIES TIMEOUT 300)

    # A broker that accepts one connection and exits at once (relaunch back-off test).
    add_executable(cml_test_fake_broker agent/fake_broker.cpp)
    target_link_libraries(cml_test_fake_broker PRIVATE cml_ipc)
    cml_apply_warnings(cml_test_fake_broker)
    set_target_properties(cml_test_fake_broker PROPERTIES FOLDER "tests")

    # A harmless DLL the end-to-end test loads and unloads to exercise ModuleLoaded/Unloaded.
    add_library(cml_test_dummy_module SHARED agent/dummy_module.cpp)
    cml_apply_warnings(cml_test_dummy_module)
    set_target_properties(cml_test_dummy_module PROPERTIES FOLDER "tests")

    # WP-08: the agent's patch loader inside a test host (needs cml_test_dummy_module above).
    add_subdirectory(agent_loader)

    # A fake cml_agent.dll with the wrong ABI version, in its own directory next to the test
    # exe, for the "ABI mismatch fails gracefully" test.
    add_library(cml_test_fake_agent_abi SHARED agent/fake_agent_abi.cpp)
    cml_apply_warnings(cml_test_fake_agent_abi)
    set_target_properties(cml_test_fake_agent_abi PROPERTIES
        OUTPUT_NAME cml_agent
        RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:cml_test_agent>/fake_agent_abi"
        # Same output name as the real agent: keep its import library and PDB out of the way.
        ARCHIVE_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:cml_test_agent>/fake_agent_abi"
        PDB_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:cml_test_agent>/fake_agent_abi"
        COMPILE_PDB_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:cml_test_agent>/fake_agent_abi"
        FOLDER "tests")
endif()
if(WIN32)
    add_subdirectory(header)
endif()

# cml_broker --version: exit code 0 and exact output.
string(CONCAT _cml_expected_version "cml_broker ${PROJECT_VERSION}")
add_test(NAME broker.version
    COMMAND ${CMAKE_COMMAND}
        "-DCML_EXE=$<TARGET_FILE:cml_broker>"
        "-DCML_ARGS=--version"
        "-DCML_EXPECT_RC=0"
        "-DCML_EXPECT_OUT=${_cml_expected_version}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/run_and_check.cmake")
add_test(NAME broker.unknown_option_fails
    COMMAND ${CMAKE_COMMAND}
        "-DCML_EXE=$<TARGET_FILE:cml_broker>"
        "-DCML_ARGS=--no-such-option"
        "-DCML_EXPECT_RC=2"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/run_and_check.cmake")
