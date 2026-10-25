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
    SOURCES util/test_diag.cpp util/test_result.cpp util/test_log.cpp util/test_hash.cpp
    LIBS cml_util)
if(NOT MSVC)
    find_package(Threads REQUIRED)
    target_link_libraries(cml_test_util PRIVATE Threads::Threads)
endif()
cml_add_unit_test(coff SOURCES lib/test_coff.cpp LIBS cml_coff)
cml_add_unit_test(pe SOURCES lib/test_pe.cpp LIBS cml_pe)
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
cml_add_unit_test(toolchain SOURCES lib/test_toolchain.cpp LIBS cml_toolchain)
cml_add_unit_test(watch SOURCES lib/test_watch.cpp LIBS cml_watch)
cml_add_unit_test(agent SOURCES agent/test_agent.cpp LIBS cml_agent)
cml_add_unit_test(broker_core SOURCES broker/test_core.cpp LIBS cml_core)

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
