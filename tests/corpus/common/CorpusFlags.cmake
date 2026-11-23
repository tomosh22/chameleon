# WP-26: the build flags docs/BUILD_REQUIREMENTS.md (plan 4.2) asks of a project that wants to be hot-reloaded, as
# CMake directory options (they come after CMAKE_<LANG>_FLAGS_<CONFIG>, so /Ob0 beats RelWithDebInfo's /Ob1).
#   compile: /Gy (function-level linking), /hotpatch (first instruction at least 2 bytes), /Ob0 (no stale inlined copies)
#   link:    /DEBUG:FULL, /INCREMENTAL:NO, /OPT:NOREF /OPT:NOICF (every function stays a real body), /FUNCTIONPADMIN
# Not used: /GL, /guard:cf. Include this file BEFORE add_subdirectory() of the project.
if(MSVC)
    # /hotpatch: clang-cl only, decided per language (cl on x64 prints D9002; /FUNCTIONPADMIN gives the padding; the corpus
    # includes C projects, where CMAKE_CXX_COMPILER_ID is not set)
    add_compile_options(/Gy "$<$<COMPILE_LANG_AND_ID:C,Clang>:/hotpatch>" "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:/hotpatch>")
    # drop CMake's /Ob1-/Ob3 from RelWithDebInfo so that /Ob0 does not trigger D9025 (Release keeps its optimisation)
    foreach(_cfg RELWITHDEBINFO)
        foreach(_lang C CXX)
            if(DEFINED CMAKE_${_lang}_FLAGS_${_cfg})
                string(REGEX REPLACE "(^| )[/-]Ob[1-3]( |$)" "\\1" CMAKE_${_lang}_FLAGS_${_cfg} "${CMAKE_${_lang}_FLAGS_${_cfg}}")
                string(REGEX REPLACE "(^| )[/-]Ob[1-3]( |$)" "\\1" CMAKE_${_lang}_FLAGS_${_cfg} "${CMAKE_${_lang}_FLAGS_${_cfg}}")
            endif()
        endforeach()
    endforeach()
    add_compile_options("$<$<CONFIG:RelWithDebInfo>:/Ob0>")
    add_link_options(/DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN)
endif()
