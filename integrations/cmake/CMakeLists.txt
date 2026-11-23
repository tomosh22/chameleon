# WP-29: configure-time tests of chameleon_enable() (ChameleonEnable.cmake). Added by the top-level CMakeLists.txt when
# CML_BUILD_TESTS is ON. Each test configures the tiny project in tests/project (no build) with the generator and compiler
# of this build and checks the diagnostic ids it prints. The examples in examples/ are not part of this tree: CI builds them
# against an install tree (.github/workflows/ci.yml, steps "WP-29 ...").
if(NOT WIN32)
    return()
endif()

set(_cml_case_script "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_case.cmake")

# cml_add_enable_test(<case> [EXPECT_ID id] [NOT_ID id] [FAILS])
function(cml_add_enable_test case)
    cmake_parse_arguments(T "FAILS" "EXPECT_ID;NOT_ID" "" ${ARGN})
    set(args
        -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests/project
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/case_${case}
        -DCASE=${case}
        -DMODULE=${CMAKE_CURRENT_SOURCE_DIR}/ChameleonEnable.cmake
        -DGENERATOR=${CMAKE_GENERATOR}
        -DPLATFORM=${CMAKE_GENERATOR_PLATFORM}
        -DCOMPILER=${CMAKE_CXX_COMPILER}
        -DBUILD_TYPE=${CMAKE_BUILD_TYPE})
    if(T_FAILS)
        list(APPEND args -DEXPECT_FAILURE=1)
    endif()
    if(T_EXPECT_ID)
        list(APPEND args -DEXPECT_ID=${T_EXPECT_ID})
    endif()
    if(T_NOT_ID)
        list(APPEND args -DNOT_ID=${T_NOT_ID})
    endif()
    add_test(NAME cmake.enable.${case} COMMAND ${CMAKE_COMMAND} ${args} -P "${_cml_case_script}")
    set_tests_properties(cmake.enable.${case} PROPERTIES TIMEOUT 120)
endfunction()

cml_add_enable_test(good NOT_ID CML17)
cml_add_enable_test(ipo EXPECT_ID CML1705)
cml_add_enable_test(guard_cf EXPECT_ID CML1706)
cml_add_enable_test(guard_cf_dir EXPECT_ID CML1706)
cml_add_enable_test(imported_target FAILS EXPECT_ID CML1708)
cml_add_enable_test(interface_library EXPECT_ID CML1702)
cml_add_enable_test(unknown_option FAILS EXPECT_ID CML1709)
cml_add_enable_test(no_target FAILS EXPECT_ID CML1703)
cml_add_enable_test(no_agent_target FAILS EXPECT_ID CML1710)
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    # Multi-configuration generators have no CMAKE_BUILD_TYPE to check.
    cml_add_enable_test(release_build EXPECT_ID CML1704)
endif()
