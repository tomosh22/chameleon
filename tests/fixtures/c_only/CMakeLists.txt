# Fixture `c_only`: a C99 translation unit that uses the C API of Chameleon_API.h
# (create the agent, enable the module, sync point in a loop). Needs the C language, which
# tests/CMakeLists.txt enables on Windows.
cml_add_fixture(c_only C SOURCES main.c)

add_test(NAME fixture.c_only.runs COMMAND fixture_c_only --ticks 3)
set_tests_properties(fixture.c_only.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=7"
    TIMEOUT 30)
