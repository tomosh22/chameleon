# Header quality gate for agent/include/Chameleon_API.h (WP-11 acceptance).
#
# The header must compile without a single warning as C99, C++14 and C++20 under both compilers at
# the maximum warning levels:
#   cl        /W4 /WX            (C is built with /std:c11, the closest mode cl offers to C99)
#   clang-cl  /W4 /WX plus -Wall -Wextra -Werror   (C is built with -std=c99)
# and with WIN32_LEAN_AND_MEAN / NOMINMAX both defined (as the rest of this project does) and
# undefined (the "_bare" variants). Each variant is an executable that is also run by ctest: it
# calls every API function on invalid input and checks that it fails gracefully, so the gate covers
# compiling, linking and the error paths of the header. No -Wno-* flag is used.

set(_cml_api_include "${PROJECT_SOURCE_DIR}/agent/include")

function(cml_add_header_check name source standard)
    cmake_parse_arguments(H "BARE" "" "" ${ARGN})
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE "${_cml_api_include}")
    set_target_properties(${name} PROPERTIES FOLDER "tests/header")
    if(H_BARE)
        target_compile_definitions(${name} PRIVATE CML_HEADER_CHECK_BARE=1)
    endif()

    if(standard STREQUAL "c99")
        if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${name} PRIVATE
                /W4 /WX /clang:-std=c99 /clang:-Wall /clang:-Wextra /clang:-Werror -Wno-unused-command-line-argument)
        else()
            target_compile_options(${name} PRIVATE /W4 /WX /std:c11)
        endif()
    else()
        # C++14 or C++20, exceptions and RTTI off like the rest of core/agent.
        set_target_properties(${name} PROPERTIES CXX_STANDARD ${standard} CXX_STANDARD_REQUIRED ON
                                                 CXX_EXTENSIONS OFF)
        cml_apply_warnings(${name})
        cml_target_core_style(${name})
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # Not plain -Wall: clang-cl maps that to -Weverything. /clang: hands the flag to clang itself.
            target_compile_options(${name} PRIVATE /clang:-Wall /clang:-Wextra /clang:-Werror)
        endif()
    endif()

    add_test(NAME header.${name} COMMAND ${name})
    set_tests_properties(header.${name} PROPERTIES TIMEOUT 60)
endfunction()

cml_add_header_check(cml_header_check_c99        header_check.c   c99)
cml_add_header_check(cml_header_check_c99_bare   header_check.c   c99 BARE)
cml_add_header_check(cml_header_check_cxx14      header_check.cpp 14)
cml_add_header_check(cml_header_check_cxx14_bare header_check.cpp 14 BARE)
cml_add_header_check(cml_header_check_cxx20      header_check.cpp 20)
cml_add_header_check(cml_header_check_cxx20_bare header_check.cpp 20 BARE)
