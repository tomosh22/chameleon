# unit.toolchain: cml_toolchain tests (WP-05).
#   test_command_line.cpp       tokeniser, quoting, response files
#   test_rewrite.cpp            rewrite behaviour, PCH strategies, rule tables
#   test_command_corpus.cpp     acceptance (1): 40+ command lines (data: command_corpus*.inc)
#   test_compile_commands.cpp   JSON reader and lookup
#   test_link_command.cpp       WP-09: link command, flag refusal, response files, linker family decision
#   test_tool_and_flags.cpp     banners, build-flag detection, symbol sets, output parsing, job pool
#   (Windows) test_invoke_win.cpp         CreateProcess, cancellation, timeouts, response files
#   (Windows) test_fixture_recompile.cpp  acceptance (2): fixtures `basic` and `pch` recompiled from their PDBs

set(_cml_toolchain_tests
    ../lib/test_toolchain.cpp
    test_command_line.cpp
    test_rewrite.cpp
    test_command_corpus.cpp
    test_compile_commands.cpp
    test_tool_and_flags.cpp
    test_link_command.cpp)
if(WIN32)
    list(APPEND _cml_toolchain_tests test_invoke_win.cpp test_fixture_recompile.cpp)
endif()

cml_add_unit_test(toolchain SOURCES ${_cml_toolchain_tests} LIBS cml_toolchain)
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # cl's traditional preprocessor mangles raw string literals (backslashes, quotes) when doctest
    # stringizes a CHECK() expression; the conforming preprocessor does it right.
    target_compile_options(cml_test_toolchain PRIVATE /Zc:preprocessor)
endif()
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

if(WIN32)
    # WP-09 acceptance: every M1 fixture's edited TU is compiled, anchored, bound and linked with link.exe and
    # lld-link, and the patch PDB is read back (unit.link_patch). Writes <build>/test_work/link_patch/evidence.txt,
    # which CI prints. Needs both linkers for the "other linker" half; skips (and says so) when one is missing.
    set(_cml_link_fixtures basic statics virtuals templates lambdas new_function new_global)
    set(_lp_paths_file "${CMAKE_CURRENT_BINARY_DIR}/link_patch_fixture_paths_$<CONFIG>.txt")
    set(_lp_content "config=$<CONFIG>\n")
    foreach(_fx IN LISTS _cml_link_fixtures)
        string(APPEND _lp_content "${_fx}_objs=$<TARGET_OBJECTS:fixture_${_fx}_objs>\n")
        string(APPEND _lp_content "${_fx}_exe=$<TARGET_FILE:fixture_${_fx}>\n")
        string(APPEND _lp_content "${_fx}_pdb=$<TARGET_PDB_FILE:fixture_${_fx}>\n")
    endforeach()
    file(GENERATE OUTPUT "${_lp_paths_file}" CONTENT "${_lp_content}")

    cml_add_unit_test(link_patch SOURCES test_link_fixtures.cpp LIBS cml_toolchain cmltest_core)
    target_include_directories(cml_test_link_patch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../support")
    target_compile_definitions(cml_test_link_patch PRIVATE
        "CML_TEST_PATHS_FILE=\"${_lp_paths_file}\""
        "CML_TEST_WORK_DIR=\"${CMAKE_BINARY_DIR}/test_work\""
        "CML_TEST_FIXTURES_DIR=\"${PROJECT_SOURCE_DIR}/tests/fixtures\""
        "CML_TEST_CXX_COMPILER=\"${CMAKE_CXX_COMPILER}\""
        "CML_TEST_LINKER=\"${CMAKE_LINKER}\"")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_definitions(cml_test_link_patch PRIVATE CML_TEST_COMPILER_IS_CL=1)
    endif()
    # Where link.exe is, independent of PATH (the Visual Studio generator job has no developer environment).
    if(CMAKE_LINKER MATCHES "[/\\\\]link\\.exe$")
        target_compile_definitions(cml_test_link_patch PRIVATE "CML_TEST_MSVC_LINK=\"${CMAKE_LINKER}\"")
    else()
        get_filename_component(_lp_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        if(EXISTS "${_lp_cxx_dir}/link.exe")
            target_compile_definitions(cml_test_link_patch PRIVATE "CML_TEST_MSVC_LINK=\"${_lp_cxx_dir}/link.exe\"")
        endif()
    endif()
    foreach(_fx IN LISTS _cml_link_fixtures)
        add_dependencies(cml_test_link_patch fixture_${_fx} fixture_${_fx}_objs)
    endforeach()
    set_tests_properties(unit.link_patch PROPERTIES TIMEOUT 900)
endif()
