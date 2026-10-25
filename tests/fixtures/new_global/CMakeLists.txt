# Fixture `new_global`: the edit adds a global with a dynamic initialiser and a destructor
# (std::string member) and uses it from compute_value(). value=1; after the edit value=7.
cml_add_fixture(new_global SOURCES main.cpp logic.cpp)

add_test(NAME fixture.new_global.runs COMMAND fixture_new_global --ticks 3)
set_tests_properties(fixture.new_global.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=1"
    TIMEOUT 30)
