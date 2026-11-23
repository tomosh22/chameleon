# chameleon_enable(<target> [NO_AGENT] [ALLOW_INLINING] [NO_COPY_AGENT])
#
# Applies the build flags Chameleon needs (docs/BUILD_REQUIREMENTS.md) to a target and links the agent.
# This one file works in two places:
#   - in the Chameleon source tree:  include(integrations/cmake/ChameleonEnable.cmake)  (the fixtures do this)
#   - from an install tree:          find_package(Chameleon CONFIG REQUIRED)  (ChameleonConfig.cmake includes it)
#
# Flags (cl and clang-cl, Ninja and Visual Studio generators, every configuration):
#   compile   /Gy /hotpatch                function-level linking, 6 bytes of padding before functions (CML1105, CML1601)
#             /Ob0                         every configuration but Debug (Debug is /Od): no inlining (CML1603)
#   link      /DEBUG:FULL /INCREMENTAL:NO  full PDB (CML1101)
#             /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN                                              (CML1602, CML1601)
# Link flags go to executables, shared libraries and modules; static and object libraries get the compile flags only.
# Agent: Chameleon::agent is linked (PRIVATE; include directory for Chameleon_API.h, the import library) and, for an
# executable, cml_agent.dll is copied next to it after the build.
#
# Options:
#   NO_AGENT         do not link Chameleon::agent and do not look for it (the target still gets the flags)
#   ALLOW_INLINING   leave the optimiser's inlining alone (a target you iterate on should keep it off: CML1603 / CML3104)
#   NO_COPY_AGENT    link the agent but do not copy cml_agent.dll beside the executable
#
# Configure-time diagnostics: CML1700-CML1719, each message ends with "What to do: ...". See integrations/cmake/README.md.

include_guard(GLOBAL)

# _cml_enable_message(<FATAL_ERROR|WARNING> <id> <what> <what to do>)
function(_cml_enable_message severity id what todo)
    message(${severity} "${id}: ${what} What to do: ${todo}")
endfunction()

function(chameleon_enable target)
    cmake_parse_arguments(PARSE_ARGV 1 E "NO_AGENT;ALLOW_INLINING;NO_COPY_AGENT" "" "")
    if(E_UNPARSED_ARGUMENTS)
        _cml_enable_message(FATAL_ERROR CML1709
            "chameleon_enable(${target}) got unknown arguments '${E_UNPARSED_ARGUMENTS}'."
            "use only NO_AGENT, ALLOW_INLINING and NO_COPY_AGENT.")
        return()
    endif()
    if(NOT TARGET ${target})
        _cml_enable_message(FATAL_ERROR CML1703
            "chameleon_enable(${target}): there is no target of that name."
            "call chameleon_enable after add_executable/add_library for the target.")
        return()
    endif()
    get_target_property(_aliased ${target} ALIASED_TARGET)
    if(_aliased)
        set(target ${_aliased})
    endif()

    # ---- the target platform: Windows x64 ----------------------------------------------------------------------------
    set(_in_tree_portable FALSE)
    if(CML_ALLOW_NON_WINDOWS)
        set(_in_tree_portable TRUE)  # the Chameleon source tree configured only for syntax checks (not supported)
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows" OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8
       OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|x64|X64)$")
        if(_in_tree_portable)
            if(CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
                target_compile_options(${target} PRIVATE -fno-inline)
            endif()
            return()
        endif()
        _cml_enable_message(FATAL_ERROR CML1700
            "chameleon_enable(${target}): the target is '${CMAKE_SYSTEM_NAME}' / '${CMAKE_SYSTEM_PROCESSOR}' with ${CMAKE_SIZEOF_VOID_P}-byte pointers; Chameleon supports Windows x64 only."
            "configure with an x64 MSVC or clang-cl toolchain (Visual Studio generator: -A x64; Ninja: run from an x64 developer prompt).")
        return()
    endif()

    # ---- the compiler: cl or clang-cl --------------------------------------------------------------------------------
    if(NOT MSVC)
        _cml_enable_message(FATAL_ERROR CML1701
            "chameleon_enable(${target}): the C++ compiler is ${CMAKE_CXX_COMPILER_ID} with the GNU-style command line; Chameleon needs cl or clang-cl (CodeView PDBs, MSVC link)."
            "configure with cl or clang-cl (clang-cl: -DCMAKE_CXX_COMPILER=clang-cl, not clang++).")
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19.30)
        _cml_enable_message(WARNING CML1707
            "cl ${CMAKE_CXX_COMPILER_VERSION} is older than Visual Studio 2022 (19.30); it is not tested with Chameleon."
            "use Visual Studio 2022 (MSVC v143) or newer.")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
        _cml_enable_message(WARNING CML1707
            "clang-cl ${CMAKE_CXX_COMPILER_VERSION} is older than LLVM 15; it is not tested with Chameleon."
            "use clang-cl 15 or newer.")
    endif()

    # ---- the target type ---------------------------------------------------------------------------------------------
    get_target_property(_type ${target} TYPE)
    if(NOT _type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY|STATIC_LIBRARY|OBJECT_LIBRARY)$")
        _cml_enable_message(WARNING CML1702
            "chameleon_enable(${target}): a ${_type} target has no compiled code of its own; no flags were applied."
            "call chameleon_enable on the executables and libraries that contain the code you reload.")
        return()
    endif()
    if(_type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
        set(_links TRUE)
    else()
        set(_links FALSE)
    endif()

    # ---- the build type (single-configuration generators) ------------------------------------------------------------
    get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NOT _multi AND NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
        _cml_enable_message(WARNING CML1704
            "chameleon_enable(${target}): CMAKE_BUILD_TYPE is '${CMAKE_BUILD_TYPE}'. Release and MinSizeRel record no debug information for the compiler, and their inlining defeats patching."
            "configure with -DCMAKE_BUILD_TYPE=Debug or RelWithDebInfo for the build you iterate on.")
    endif()

    # ---- settings Chameleon rejects (CML1101, CML1103, CML1104, CML1504) ----------------------------------------------
    get_target_property(_ipo ${target} INTERPROCEDURAL_OPTIMIZATION)
    if(_ipo OR CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        _cml_enable_message(WARNING CML1705
            "chameleon_enable(${target}): interprocedural optimisation (/GL, LTCG) is on; it is switched off for this target because `cml_broker check` fails /GL builds (CML1103)."
            "do not set INTERPROCEDURAL_OPTIMIZATION or CMAKE_INTERPROCEDURAL_OPTIMIZATION for the targets you reload.")
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
    endif()
    set(_seen "${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS_DEBUG} ${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    string(APPEND _seen " ${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS_DEBUG} ${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}")
    foreach(_prop COMPILE_OPTIONS LINK_OPTIONS)
        get_target_property(_val ${target} ${_prop})
        if(_val)
            string(APPEND _seen " ${_val}")
        endif()
    endforeach()
    string(TOLOWER "${_seen}" _seen)
    string(REPLACE " " ";" _seen "${_seen}")
    set(_bad "")
    foreach(_tok IN LISTS _seen)
        if(_tok MATCHES "^[/-]guard:cf$")
            list(APPEND _bad "${_tok} (CML1104)")
        elseif(_tok MATCHES "^[/-](gl|ltcg(:.*)?)$")
            list(APPEND _bad "${_tok} (CML1103)")
        elseif(_tok MATCHES "^[/-](clr(:.*)?|zw)$")
            list(APPEND _bad "${_tok} (CML1504)")
        elseif(_tok MATCHES "^[/-]debug:fastlink$")
            list(APPEND _bad "${_tok} (CML1101)")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _bad)
    if(_bad)
        _cml_enable_message(WARNING CML1706
            "chameleon_enable(${target}): the flags in effect contain options `cml_broker check` rejects: ${_bad}."
            "remove them from CMAKE_*_FLAGS, CMAKE_*_LINKER_FLAGS and the target's options for the targets you reload.")
    endif()

    # ---- the flags ---------------------------------------------------------------------------------------------------
    # Ninja and Makefile generators pass compile options to every language, resource compilers included, so the options
    # are restricted to C and C++ with single-language genexes. The Visual Studio generators put compile options into
    # the ClCompile item only (and restrict $<COMPILE_LANGUAGE> to single-language targets), so they get plain options.
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        target_compile_options(${target} PRIVATE /Gy /hotpatch)
        if(NOT E_ALLOW_INLINING)
            target_compile_options(${target} PRIVATE "$<$<NOT:$<CONFIG:Debug>>:/Ob0>")
        endif()
    else()
        foreach(_lang C CXX)
            target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:${_lang}>:/Gy>" "$<$<COMPILE_LANGUAGE:${_lang}>:/hotpatch>")
            if(NOT E_ALLOW_INLINING)
                target_compile_options(${target} PRIVATE "$<$<AND:$<COMPILE_LANGUAGE:${_lang}>,$<NOT:$<CONFIG:Debug>>>:/Ob0>")
            endif()
        endforeach()
    endif()
    if(_links)
        target_link_options(${target} PRIVATE /DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN)
    endif()

    # ---- the agent ---------------------------------------------------------------------------------------------------
    if(NOT E_NO_AGENT)
        if(NOT TARGET Chameleon::agent)
            _cml_enable_message(FATAL_ERROR CML1710
                "chameleon_enable(${target}): the target Chameleon::agent does not exist."
                "call find_package(Chameleon CONFIG REQUIRED) (with CMAKE_PREFIX_PATH or Chameleon_DIR at the install tree) before chameleon_enable, or pass NO_AGENT.")
            return()
        endif()
        target_link_libraries(${target} PRIVATE Chameleon::agent)
        if(_type STREQUAL "EXECUTABLE" AND NOT E_NO_COPY_AGENT)
            get_target_property(_bin_dir ${target} BINARY_DIR)
            if(_bin_dir STREQUAL CMAKE_CURRENT_BINARY_DIR)
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:Chameleon::agent>" "$<TARGET_FILE_DIR:${target}>"
                    COMMENT "Chameleon: copying cml_agent.dll next to ${target}"
                    VERBATIM)
            else()
                _cml_enable_message(WARNING CML1711
                    "chameleon_enable(${target}) was called from a different directory than the one that created the target; cml_agent.dll is not copied next to it."
                    "call chameleon_enable in the directory of the add_executable call, or copy cml_agent.dll yourself (or pass NO_COPY_AGENT).")
            endif()
        endif()
    endif()
endfunction()
