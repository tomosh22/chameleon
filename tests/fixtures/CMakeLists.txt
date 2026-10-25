# Test fixtures (plan section 10). Each fixture is a console app with a 100 ms main loop that
# prints `TICK k=v ...` lines. Fixtures are built with the project's normal compiler and config;
# consumers (unit tests, cmltest) locate their outputs through generator expressions:
#   $<TARGET_FILE:fixture_basic>       the exe
#   $<TARGET_PDB_FILE:fixture_basic>   its PDB
#   $<TARGET_OBJECTS:fixture_basic_objs>  its object files (one per TU)

include("${CMAKE_CURRENT_LIST_DIR}/CmlFixture.cmake")

# Fixtures stand in for user projects, which use the normal exception-enabled STL. Undo the
# project-wide _HAS_EXCEPTIONS=0 (cmake/CmlToolchain.cmake) for this directory. Fixtures never
# link core/ static libraries, so no inline STL code is shared across the two settings.
get_directory_property(_cml_fixture_defs COMPILE_DEFINITIONS)
list(REMOVE_ITEM _cml_fixture_defs _HAS_EXCEPTIONS=0)
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_cml_fixture_defs}")

add_subdirectory(basic)
add_subdirectory(pch)  # WP-05: precompiled header (/Yc + /Yu) fixture
# c_only is C: needs the C language (enabled on Windows by tests/CMakeLists.txt).
if(CMAKE_C_COMPILER_LOADED)
    add_subdirectory(c_only)
endif()
add_subdirectory(bigobj)
