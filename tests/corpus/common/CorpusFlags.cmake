# WP-26: the build flags docs/BUILD_REQUIREMENTS.md (plan 4.2) asks of a project that wants to be hot-reloaded, as
# CMake directory options (they come after CMAKE_<LANG>_FLAGS_<CONFIG>, so /Ob0 beats RelWithDebInfo's /Ob1).
#   compile: /Gy (function-level linking), /hotpatch (first instruction at least 2 bytes), /Ob0 (no stale inlined copies)
#   link:    /DEBUG:FULL, /INCREMENTAL:NO, /OPT:NOREF /OPT:NOICF (every function stays a real body), /FUNCTIONPADMIN
# Not used: /GL, /guard:cf. Include this file BEFORE add_subdirectory() of the project.
if(MSVC)
    # /hotpatch: clang-cl only, decided per language (cl on x64 prints D9002; /FUNCTIONPADMIN gives the padding; the corpus
    # includes C projects, where CMAKE_CXX_COMPILER_ID is not set)
    add_compile_options(/Gy "$<$<COMPILE_LANG_AND_ID:C,Clang>:/hotpatch>" "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:/hotpatch>")
    # drop CMake's /Ob1 /Ob2 so that /Ob0 does not trigger D9025
    set(_cml_cfgs RELEASE RELWITHDEBINFO MINSIZEREL)
    foreach(_cfg ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
        string(TOUPPER "${_cfg}" _cfg)
        list(APPEND _cml_cfgs ${_cfg})
    endforeach()
    list(REMOVE_DUPLICATES _cml_cfgs)
    list(REMOVE_ITEM _cml_cfgs DEBUG)
    foreach(_cfg IN LISTS _cml_cfgs)
        foreach(_lang C CXX)
            if(DEFINED CMAKE_${_lang}_FLAGS_${_cfg})
                string(REGEX REPLACE "(^| )[/-]Ob[1-3]( |$)" "\\1" CMAKE_${_lang}_FLAGS_${_cfg} "${CMAKE_${_lang}_FLAGS_${_cfg}}")
                string(REGEX REPLACE "(^| )[/-]Ob[1-3]( |$)" "\\1" CMAKE_${_lang}_FLAGS_${_cfg} "${CMAKE_${_lang}_FLAGS_${_cfg}}")
            endif()
        endforeach()
    endforeach()
    add_compile_options("$<$<NOT:$<CONFIG:Debug>>:/Ob0>")  # every stripped configuration
    add_link_options(/DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN)
endif()
