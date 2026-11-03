# Fixture `bad_builds` (WP-18): one tiny program built the ways Chameleon rejects (plan 1 and 4.2), plus ctest tests that
# run `cml_broker check` on them and on the good fixtures. A negative case is a test that PASSES when the audit says FAIL with
# the right id (exit code 1 and the id in the output).
#
#   bad_build_guard_cf    /guard:cf at compile and link          CML1104
#   bad_build_fastlink    /DEBUG:FASTLINK                        CML1101   (link.exe only: lld-link ignores FASTLINK)
#   bad_build_gl          /GL (cl only: clang-cl's -flto objects need lld-link)    CML1103
#   bad_build_no_pdb      the module copied without its PDB      CML1410
#   bad_build_no_command  clang-cl -gno-codeview-command-line    CML1102   (no S_ENVBLOCK / command line recorded)
#
# The cases that need a compiler or linker feature the CI cell lacks are not built there; the unit tests
# (tests/unit/broker/test_audit.cpp) cover every id on synthetic modules in every cell.
if(NOT MSVC)
    return()
endif()

set(_cml_bad_good_compile /Gy /hotpatch)
set(_cml_bad_good_link /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN)
set(_cml_check_script "${CMAKE_CURRENT_SOURCE_DIR}/run_check.cmake")

# cml_add_bad_build(<name> [COMPILE opts...] [LINK opts...] [DEBUG_MODE FULL|FASTLINK])
function(cml_add_bad_build name)
    cmake_parse_arguments(B "" "DEBUG_MODE" "COMPILE;LINK" ${ARGN})
    if(NOT B_DEBUG_MODE)
        set(B_DEBUG_MODE FULL)
    endif()
    set(target bad_build_${name})
    add_executable(${target} main.cpp)
    target_compile_options(${target} PRIVATE ${_cml_bad_good_compile} ${B_COMPILE})
    target_link_options(${target} PRIVATE /DEBUG:${B_DEBUG_MODE} ${_cml_bad_good_link} ${B_LINK})
    set_target_properties(${target} PROPERTIES FOLDER "tests/fixtures")
endfunction()

# cml_add_check_test(<test name> MODULE <file> EXIT <0|1> [LINES a;b] [NOT_LINE x] [ALONE_DIR dir])
function(cml_add_check_test name)
    cmake_parse_arguments(T "" "MODULE;EXIT;NOT_LINE;ALONE_DIR" "LINES;DEPENDS" ${ARGN})
    set(args -DBROKER=$<TARGET_FILE:cml_broker> -DMODULE=${T_MODULE} -DEXPECT_EXIT=${T_EXIT})
    if(T_LINES)
        list(JOIN T_LINES "\\;" _lines)
        list(APPEND args "-DEXPECT_LINE=${_lines}")
    endif()
    if(T_NOT_LINE)
        list(APPEND args "-DNOT_LINE=${T_NOT_LINE}")
    endif()
    if(T_ALONE_DIR)
        list(APPEND args "-DCOPY_ALONE_TO=${T_ALONE_DIR}")
    endif()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${args} -P "${_cml_check_script}")
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

if(NOT TARGET cml_broker)
    return()
endif()

# ---- the good fixtures must pass (exit code 0, no FAIL line) --------------------------------------------------------
if(TARGET fixture_basic)
    cml_add_check_test(check.fixture_basic MODULE $<TARGET_FILE:fixture_basic> EXIT 0 NOT_LINE "FAIL CML")
endif()
if(TARGET fixture_optimised)
    # Optimised with inlining: passes, with the recommendation WARN CML1603.
    cml_add_check_test(check.fixture_optimised MODULE $<TARGET_FILE:fixture_optimised> EXIT 0 LINES "WARN CML1603" NOT_LINE "FAIL CML")
endif()

# ---- /guard:cf ---------------------------------------------------------------------------------------------------------
cml_add_bad_build(guard_cf COMPILE /guard:cf LINK /GUARD:CF)
cml_add_check_test(check.bad_build.guard_cf MODULE $<TARGET_FILE:bad_build_guard_cf> EXIT 1 LINES "FAIL CML1104")

# ---- /DEBUG:FASTLINK (lld-link ignores it and writes a full PDB) --------------------------------------------------------
if(NOT CMAKE_LINKER MATCHES "lld-link")
    cml_add_bad_build(fastlink DEBUG_MODE FASTLINK)
    cml_add_check_test(check.bad_build.fastlink MODULE $<TARGET_FILE:bad_build_fastlink> EXIT 1 LINES "FAIL CML1101")
endif()

# ---- /GL (cl) ------------------------------------------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    cml_add_bad_build(gl COMPILE /GL LINK /LTCG)
    cml_add_check_test(check.bad_build.gl MODULE $<TARGET_FILE:bad_build_gl> EXIT 1 LINES "FAIL CML1103")
endif()

# ---- missing PDB: the PE records only the bare file name (/PDBALTPATH), and the test copies the exe away alone -------
cml_add_bad_build(no_pdb LINK /PDBALTPATH:bad_build_no_pdb.pdb)
cml_add_check_test(check.bad_build.no_pdb MODULE $<TARGET_FILE:bad_build_no_pdb> EXIT 1 LINES "FAIL CML1410"
    ALONE_DIR "${CMAKE_CURRENT_BINARY_DIR}/no_pdb_scratch")

# ---- no recorded command line (clang-cl: S_BUILDINFO without the command) ------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("/clang:-gno-codeview-command-line" CML_HAS_NO_CV_COMMAND_LINE)
    if(CML_HAS_NO_CV_COMMAND_LINE)
        cml_add_bad_build(no_command COMPILE /clang:-gno-codeview-command-line)
        cml_add_check_test(check.bad_build.no_command MODULE $<TARGET_FILE:bad_build_no_command> EXIT 1 LINES "FAIL CML1102")
    endif()
endif()
