# Warning policy helpers.
#
#   cml_apply_warnings(<target>)         strict: /W4 /WX (MSVC cl and clang-cl)
#   cml_apply_relaxed_warnings(<target>) for vendored third-party code: never /WX

function(cml_apply_warnings target)
    if(MSVC)
        # MSVC is true for both cl and clang-cl.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # clang-cl: driver-level "argument unused" diagnostics become errors under /WX,
            # so silence those first. /W4 in clang-cl maps to -Wall -Wextra. Keep it strict;
            # add targeted -Wno-xxx here only with a comment saying why.
            target_compile_options(${target} PRIVATE
                -Wno-unused-command-line-argument
                /W4 /WX /permissive- /utf-8)
        else()
            target_compile_options(${target} PRIVATE
                /W4 /WX /permissive- /utf-8 /Zc:__cplusplus
                # Headers found through SYSTEM include directories (third_party/) are not warned about.
                /external:W0)
        endif()
    else()
        # Only reachable with CML_ALLOW_NON_WINDOWS (portable syntax checking).
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

function(cml_apply_relaxed_warnings target)
    if(MSVC)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${target} PRIVATE -Wno-unused-command-line-argument -w /utf-8)
        else()
            target_compile_options(${target} PRIVATE /W3 /utf-8)
        endif()
    else()
        target_compile_options(${target} PRIVATE -w)
    endif()
endfunction()
