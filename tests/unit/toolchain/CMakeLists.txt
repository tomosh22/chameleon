# unit.toolchain: cml_toolchain tests (WP-05).
#   test_command_line.cpp       tokeniser, quoting, response files
#   test_rewrite.cpp            rewrite behaviour, PCH strategies, rule tables
#   test_command_corpus.cpp     acceptance (1): 40+ command lines (data: command_corpus*.inc)
#   test_compile_commands.cpp   JSON reader and lookup
#   test_tool_and_flags.cpp     banners, build-flag detection, symbol sets, output parsing, job pool
#   (Windows) test_invoke_win.cpp         CreateProcess, cancellation, timeouts, response files
#   (Windows) test_fixture_recompile.cpp  acceptance (2): fixtures `basic` and `pch` recompiled from their PDBs

set(_cml_toolchain_tests
    ../lib/test_toolchain.cpp
    test_command_line.cpp
    test_rewrite.cpp
    test_command_corpus.cpp
    test_compile_commands.cpp
    test_tool_and_flags.cpp)
if(WIN32)
    list(APPEND _cml_toolchain_tests test_invoke_win.cpp test_fixture_recompile.cpp)
endif()

cml_add_unit_test(toolchain SOURCES ${_cml_toolchain_tests} LIBS cml_toolchain)
target_include_directories(cml_test_toolchain PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../support")

if(WIN32)
    # Fixture output locations for test_fixture_recompile.cpp (same mechanism as unit.coff).
    set(_paths_file "${CMAKE_CURRENT_BINARY_DIR}/toolchain_fixture_paths_$<CONFIG>.txt")
    file(GENERATE OUTPUT "${_paths_file}" CONTENT
"config=$<CONFIG>
basic_objs=$<TARGET_OBJECTS:fixture_basic_objs>
basic_exe=$<TARGET_FILE:fixture_basic>
basic_pdb=$<TARGET_PDB_FILE:fixture_basic>
pch_objs=$<TARGET_OBJECTS:fixture_pch_objs>
pch_exe=$<TARGET_FILE:fixture_pch>
pch_pdb=$<TARGET_PDB_FILE:fixture_pch>
")
    target_compile_definitions(cml_test_toolchain PRIVATE
        "CML_TEST_PATHS_FILE=\"${_paths_file}\""
        "CML_TEST_WORK_DIR=\"${CMAKE_BINARY_DIR}/test_work\""
        "CML_TEST_CXX_COMPILER=\"${CMAKE_CXX_COMPILER}\"")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_definitions(cml_test_toolchain PRIVATE CML_TEST_COMPILER_IS_CL=1)  # cl.exe, not clang-cl
    endif()
    add_dependencies(cml_test_toolchain fixture_basic fixture_basic_objs fixture_pch fixture_pch_objs)
    set_tests_properties(unit.toolchain PROPERTIES TIMEOUT 600)
endif()
