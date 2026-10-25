# cml_add_fixture(<name> [C] SOURCES ...)
#
# Creates an OBJECT library fixture_<name>_objs (so tests can read the per-TU .obj files) and an
# executable fixture_<name> linked from it. Flags follow docs/BUILD_REQUIREMENTS.md: full PDBs,
# function-level linking, hot-patchable entries with padding, no COMDAT folding or dead-stripping.
# Fixtures deliberately use the compiler's ordinary Debug/RelWithDebInfo flags otherwise (/RTC1,
# /GS, ...) because that is what real user projects look like.
#
# "Linked with the agent" (plan section 10) means: every fixture can use agent/include/Chameleon_API.h
# and finds cml_agent.dll and cml_broker.exe next to its own exe. The fixture never links the agent
# (the header LoadLibrary()s it); this function only adds the include directory and build-order
# dependencies. cml_agent.dll and cml_broker.exe land beside the fixture because the whole project
# builds into one runtime output directory (CMAKE_RUNTIME_OUTPUT_DIRECTORY in the top-level
# CMakeLists.txt, which is also used for DLLs).
#
# The C keyword marks a fixture whose sources are C: C++-only options (/EHsc, /GR) are skipped.
function(cml_add_fixture name)
    cmake_parse_arguments(F "C" "" "SOURCES" ${ARGN})
    set(objs fixture_${name}_objs)
    add_library(${objs} OBJECT ${F_SOURCES})
    if(NOT F_C)
        cml_target_exceptions_allowed(${objs})
    endif()
    cml_target_fixture_style(${objs})
    # CML_AGENT_INCLUDE_DIR (WP-13): set by tests/harness/fixture_wrapper when cmltest builds a fixture
    # standalone; in the main tree it is unset and the agent header comes from this repository.
    if(CML_AGENT_INCLUDE_DIR)
        set(_cml_agent_include "${CML_AGENT_INCLUDE_DIR}")
    else()
        set(_cml_agent_include "${PROJECT_SOURCE_DIR}/agent/include")
    endif()
    target_include_directories(${objs} PRIVATE "${_cml_agent_include}")
    if(MSVC)
        target_compile_options(${objs} PRIVATE /Gy /hotpatch)
    endif()
    set_target_properties(${objs} PROPERTIES FOLDER "tests/fixtures")

    add_executable(fixture_${name} $<TARGET_OBJECTS:${objs}>)
    # Fixtures load the agent at run time; make sure it and the broker are built before any test
    # runs the fixture.
    if(TARGET cml_agent AND TARGET cml_broker)
        add_dependencies(fixture_${name} cml_agent cml_broker)
    endif()
    if(MSVC)
        target_link_options(fixture_${name} PRIVATE
            /DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN)
    endif()
    set_target_properties(fixture_${name} PROPERTIES FOLDER "tests/fixtures")
endfunction()
