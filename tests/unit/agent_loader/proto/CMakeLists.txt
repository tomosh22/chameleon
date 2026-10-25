# The prototype patch DLL of unit.agent_loader: no CRT, no imports, /FIXED at a placeholder base.
#
# Why one prebuilt DLL instead of linking a variant per test base: the load addresses are only known
# at run time (they are reserved next to the test exe), and the stress test needs 500 different
# bases. A /FIXED image has no relocations and this DLL is position independent, so the tests copy
# it and rewrite ImageBase in the PE header; that needs no compiler or linker at test time, so it is
# identical under cl/link and clang-cl/lld-link, and takes microseconds. See patch_proto.cpp.
#
# This directory has its own variable scope on purpose: the settings below (no standard libraries,
# no /RTC1) must not reach the test executable.

if(MSVC)
    set(CMAKE_CXX_STANDARD_LIBRARIES "")
    foreach(_cfg DEBUG RELWITHDEBINFO RELEASE MINSIZEREL)
        string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_${_cfg} "${CMAKE_CXX_FLAGS_${_cfg}}")
    endforeach()

    add_library(cml_test_patch_proto SHARED patch_proto.cpp)
    cml_apply_warnings(cml_test_patch_proto)
    target_compile_options(cml_test_patch_proto PRIVATE /GS- /Zl)
    target_link_options(cml_test_patch_proto PRIVATE
        /NOENTRY /NODEFAULTLIB /FIXED /DYNAMICBASE:NO /INCREMENTAL:NO
        /OPT:NOREF /OPT:NOICF /BASE:0x6600000000
        # clang-cl ignores `execute` in #pragma section; the linker flag makes .cmlx RX under both linkers.
        /SECTION:.cmlx,ER)
    set_target_properties(cml_test_patch_proto PROPERTIES FOLDER "tests")
endif()
