# WP-38 (ADR-0042): code compiled for AVX2 and AVX-512, so that the hook installer's length decoder meets VEX- and
# EVEX-encoded instructions that a compiler really emits.
#
#   cml_test_hook_avx2      object library with the functions unit.agent_hook_stress hooks: compiled with /arch:AVX2 and
#                           optimised in EVERY configuration, so that their first instruction is the VEX instruction
#                           itself (a leaf function without a prologue), in the Debug cells too. The stress test checks
#                           the entry bytes at run time and fails when they are not VEX.
#   cml_lde_avx_module      an executable that is never run: its functions (/arch:AVX2, and one translation unit with
#                           /arch:AVX512) are what cml_lde_probe plans a hook for (ctest tool.lde_corpus_avx_module,
#                           every cell) and what cml_lde_diff sweeps against the reference disassembler (fuzz.yml). The
#                           AVX2 code is compiled twice: optimised here, and with the configuration's own flags in the
#                           parent directory (cl /Od homes arguments with VEX stores first).
#
# This directory's Debug flags are the Release ones as far as code generation goes: /RTC1 is incompatible with /O2
# (D8016), so it goes too. The CRT and the debug information stay the configuration's.
if(NOT MSVC)
    return()
endif()

foreach(_cml_avx_flags CMAKE_CXX_FLAGS_DEBUG)
    string(REGEX REPLACE "/RTC[1csu]+" "" ${_cml_avx_flags} "${${_cml_avx_flags}}")
    string(REGEX REPLACE "/Ob0" "" ${_cml_avx_flags} "${${_cml_avx_flags}}")
    string(REGEX REPLACE "/Od" "/O2" ${_cml_avx_flags} "${${_cml_avx_flags}}")
endforeach()

add_library(cml_test_hook_avx2 OBJECT stress_avx2.cpp)
target_compile_options(cml_test_hook_avx2 PRIVATE /arch:AVX2)
cml_apply_warnings(cml_test_hook_avx2)
cml_target_core_style(cml_test_hook_avx2)
set_target_properties(cml_test_hook_avx2 PROPERTIES FOLDER "tests")

add_library(cml_lde_avx2_optimised OBJECT lde_avx2_code.cpp)
target_compile_options(cml_lde_avx2_optimised PRIVATE /arch:AVX2)
target_compile_definitions(cml_lde_avx2_optimised PRIVATE CML_AVX_VARIANT=optimised)
add_library(cml_lde_avx512_optimised OBJECT lde_avx512_code.cpp)
target_compile_options(cml_lde_avx512_optimised PRIVATE /arch:AVX512)
foreach(_cml_avx_lib cml_lde_avx2_optimised cml_lde_avx512_optimised)
    cml_apply_warnings(${_cml_avx_lib})
    cml_target_core_style(${_cml_avx_lib})
    set_target_properties(${_cml_avx_lib} PROPERTIES FOLDER "tests")
endforeach()
