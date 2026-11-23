# cml_add_fixture(<name> [C] SOURCES ...)
#
# Creates an OBJECT library fixture_<name>_objs (so tests can read the per-TU .obj files) and an
# executable fixture_<name> linked from it. The Chameleon build flags (docs/BUILD_REQUIREMENTS.md: full PDBs,
# function-level linking, hot-patchable entries with padding, no COMDAT folding or dead-stripping, /Ob0 outside
# Debug) come from chameleon_enable() (integrations/cmake/ChameleonEnable.cmake, WP-29), the same function a user
# project calls. Fixtures pass NO_AGENT: they load cml_agent.dll at run time and take the header from
# CML_AGENT_INCLUDE_DIR below (cmltest builds them standalone, where there is no Chameleon::agent target).
# Fixtures deliberately use the compiler's ordinary Debug/RelWithDebInfo flags otherwise (/RTC1,
# /GS, ...) because that is what real user projects look like. A fixture that needs other flags
# (optimised, bad_builds) adjusts the target after this call or does not use it.
#
# "Linked with the agent" (plan section 10) means: every fixture can use agent/include/Chameleon_API.h
# and finds cml_agent.dll and cml_broker.exe next to its own exe. The fixture never links the agent
# (the header LoadLibrary()s it); this function only adds the include directory and build-order
# dependencies. cml_agent.dll and cml_broker.exe land beside the fixture because the whole project
# builds into one runtime output directory (CMAKE_RUNTIME_OUTPUT_DIRECTORY in the top-level
# CMakeLists.txt, which is also used for DLLs).
#
# The C keyword marks a fixture whose sources are C: C++-only options (/EHsc, /GR) are skipped.
include("${CMAKE_CURRENT_LIST_DIR}/../../integrations/cmake/ChameleonEnable.cmake")

# chameleon_enable() removes CMake's /Ob1 /Ob2 from CMAKE_<LANG>_FLAGS_<CONFIG> in its caller's scope (the cml_add_fixture*
# function); this macro, used inside those functions, passes the result on to the directory so /Ob0 does not trigger D9025.
macro(_cml_fixture_propagate_flags)
    foreach(_lang C CXX)
        foreach(_cfg RELEASE RELWITHDEBINFO MINSIZEREL)
            set(CMAKE_${_lang}_FLAGS_${_cfg} "${CMAKE_${_lang}_FLAGS_${_cfg}}" PARENT_SCOPE)
        endforeach()
    endforeach()
endmacro()

function(cml_add_fixture name)
    cmake_parse_arguments(F "C" "" "SOURCES" ${ARGN})
    set(objs fixture_${name}_objs)
    add_library(${objs} OBJECT ${F_SOURCES})
    if(NOT F_C)
        cml_target_exceptions_allowed(${objs})
    endif()
    chameleon_enable(${objs} NO_AGENT NO_BUILD_TYPE_CHECK)
    _cml_fixture_propagate_flags()
    # CML_AGENT_INCLUDE_DIR (WP-13): set by tests/harness/fixture_wrapper when cmltest builds a fixture
    # standalone; in the main tree it is unset and the agent header comes from this repository.
    if(CML_AGENT_INCLUDE_DIR)
        set(_cml_agent_include "${CML_AGENT_INCLUDE_DIR}")
    else()
        set(_cml_agent_include "${PROJECT_SOURCE_DIR}/agent/include")
    endif()
    # common/fixture_runtime.h: the shared TICK loop used by fixtures newer than `basic`.
    target_include_directories(${objs} PRIVATE "${_cml_agent_include}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/common")
    set_target_properties(${objs} PROPERTIES FOLDER "tests/fixtures")

    add_executable(fixture_${name} $<TARGET_OBJECTS:${objs}>)
    # Fixtures load the agent at run time; make sure it and the broker are built before any test
    # runs the fixture.
    if(TARGET cml_agent AND TARGET cml_broker)
        add_dependencies(fixture_${name} cml_agent cml_broker)
    endif()
    chameleon_enable(fixture_${name} NO_AGENT NO_BUILD_TYPE_CHECK)
    _cml_fixture_propagate_flags()
    set_target_properties(fixture_${name} PROPERTIES FOLDER "tests/fixtures")
endfunction()

# cml_add_fixture_dll(<target> SOURCES ...)   (WP-16)
#
# A DLL of a multi-module fixture, built like the fixture exe (chameleon_enable): /Gy /hotpatch, full PDB, no ICF / dead stripping,
# padded function entries. It lands beside the exe (the whole project builds into one runtime output directory,
# DLLs included), so the exe finds it through the normal DLL search and the fixture enables it by file name
# (cml_fixture::extra_modules()). The exe links its import library: `target_link_libraries(fixture_x PRIVATE <target>)`.
function(cml_add_fixture_dll target)
    cmake_parse_arguments(F "" "" "SOURCES" ${ARGN})
    add_library(${target} SHARED ${F_SOURCES})
    cml_target_exceptions_allowed(${target})
    chameleon_enable(${target} NO_AGENT NO_BUILD_TYPE_CHECK)
    _cml_fixture_propagate_flags()
    set_target_properties(${target} PROPERTIES FOLDER "tests/fixtures")
endfunction()
