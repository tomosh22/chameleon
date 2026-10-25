# cml_add_fixture(<name> SOURCES ...)
#
# Creates an OBJECT library fixture_<name>_objs (so tests can read the per-TU .obj files) and an
# executable fixture_<name> linked from it. Flags follow docs/BUILD_REQUIREMENTS.md: full PDBs,
# function-level linking, hot-patchable entries with padding, no COMDAT folding or dead-stripping.
# Fixtures deliberately use the compiler's ordinary Debug/RelWithDebInfo flags otherwise (/RTC1,
# /GS, ...) because that is what real user projects look like.
function(cml_add_fixture name)
    cmake_parse_arguments(F "" "" "SOURCES" ${ARGN})
    set(objs fixture_${name}_objs)
    add_library(${objs} OBJECT ${F_SOURCES})
    cml_target_exceptions_allowed(${objs})
    cml_target_fixture_style(${objs})
    if(MSVC)
        target_compile_options(${objs} PRIVATE /Gy /hotpatch)
    endif()
    set_target_properties(${objs} PROPERTIES FOLDER "tests/fixtures")

    add_executable(fixture_${name} $<TARGET_OBJECTS:${objs}>)
    if(MSVC)
        target_link_options(fixture_${name} PRIVATE
            /DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN)
    endif()
    set_target_properties(fixture_${name} PROPERTIES FOLDER "tests/fixtures")
endfunction()
