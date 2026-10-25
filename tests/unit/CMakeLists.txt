# Unit tests: one doctest executable per library (cml_test_<name>), each registered with ctest as
# unit.<name>. Tests may use exceptions (doctest needs them); the code under test does not.

set(CML_UNIT_MAIN "${CMAKE_CURRENT_SOURCE_DIR}/doctest_main.cpp")

function(cml_add_unit_test name)
    cmake_parse_arguments(T "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(cml_test_${name} ${CML_UNIT_MAIN} ${T_SOURCES})
    target_link_libraries(cml_test_${name} PRIVATE cml_third_party_doctest ${T_LIBS})
    cml_apply_warnings(cml_test_${name})
    cml_target_exceptions_allowed(cml_test_${name})
    set_target_properties(cml_test_${name} PROPERTIES FOLDER "tests")
    add_test(NAME unit.${name} COMMAND cml_test_${name})
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
cml_add_unit_test(ipc SOURCES lib/test_ipc.cpp LIBS cml_ipc)
cml_add_unit_test(proc SOURCES lib/test_proc.cpp LIBS cml_proc)
if(WIN32)
    # WP-11: named-pipe transport tests (real pipes, in-process client and server threads).
    target_sources(cml_test_ipc PRIVATE lib/test_ipc_pipe.cpp)
    set_tests_properties(unit.ipc unit.proc PROPERTIES TIMEOUT 120)
endif()
cml_add_unit_test(toolchain SOURCES lib/test_toolchain.cpp LIBS cml_toolchain)
cml_add_unit_test(watch SOURCES lib/test_watch.cpp LIBS cml_watch)
# WP-11: agent pieces (SyncPoint queue, module diff, heap) link the agent core library directly; the
# end-to-end tests load cml_agent.dll through Chameleon_API.h like an application would and talk to
# an in-process broker (cml_core's ConnectionServer).
cml_add_unit_test(agent
    SOURCES agent/test_sync_queue.cpp agent/test_module_diff.cpp
    LIBS cml_agent_core)
cml_add_unit_test(broker_core SOURCES broker/test_core.cpp broker/test_session.cpp LIBS cml_core)
if(NOT MSVC)
    # Portable pieces are tested on non-Windows hosts too (CML_ALLOW_NON_WINDOWS).
    target_link_libraries(cml_test_agent PRIVATE Threads::Threads)
    target_link_libraries(cml_test_broker_core PRIVATE Threads::Threads)
endif()
if(WIN32)
    target_sources(cml_test_agent PRIVATE agent/test_heap.cpp agent/test_agent_log.cpp agent/test_agent_table.cpp agent/test_agent.cpp)
    target_link_libraries(cml_test_agent PRIVATE cml_core)
    target_include_directories(cml_test_agent PRIVATE "${PROJECT_SOURCE_DIR}/agent/include")
    add_dependencies(cml_test_agent cml_agent cml_broker cml_test_dummy_module cml_test_fake_agent_abi)
    # The real broker, for the test that lets the agent auto-start one (and kills it).
    target_compile_definitions(cml_test_agent PRIVATE "CML_TEST_BROKER_EXE=\"$<TARGET_FILE:cml_broker>\"")
    target_sources(cml_test_broker_core PRIVATE broker/test_connection_server.cpp)
    set_tests_properties(unit.agent unit.broker_core PROPERTIES TIMEOUT 300)

    # A harmless DLL the end-to-end test loads and unloads to exercise ModuleLoaded/Unloaded.
    add_library(cml_test_dummy_module SHARED agent/dummy_module.cpp)
    cml_apply_warnings(cml_test_dummy_module)
    set_target_properties(cml_test_dummy_module PROPERTIES FOLDER "tests")

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
