# Fixture `static_lib` (WP-26b, ADR-0036): code that is linked from a static library. The exe's TICK loop calls
# static_lib_value(), which lives in fixture_static_lib_lib (add_library(... STATIC)); the PDB of the exe lists the library
# member as a compiland whose module names an archive. The edit changes the member's function: a reload must compile, link and
# patch it like a plain object. `calls=` is a file-static counter of the member, bound to the running process's storage.
add_library(fixture_static_lib_lib STATIC lib.cpp)
cml_target_exceptions_allowed(fixture_static_lib_lib)
cml_target_fixture_style(fixture_static_lib_lib)
if(MSVC)
    target_compile_options(fixture_static_lib_lib PRIVATE /Gy /hotpatch)
endif()
target_include_directories(fixture_static_lib_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(fixture_static_lib_lib PROPERTIES FOLDER "tests/fixtures")

cml_add_fixture(static_lib SOURCES main.cpp)
target_include_directories(fixture_static_lib_objs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fixture_static_lib PRIVATE fixture_static_lib_lib)

add_test(NAME fixture.static_lib.runs COMMAND fixture_static_lib --ticks 3)
set_tests_properties(fixture.static_lib.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=7 calls=3"
    TIMEOUT 30)
