# Fixture `new_function`: the edit adds a new external function to logic.cpp and calls it from
# the existing compute_value(). value=1; after the edit value=42.
cml_add_fixture(new_function SOURCES main.cpp logic.cpp)

add_test(NAME fixture.new_function.runs COMMAND fixture_new_function --ticks 3)
set_tests_properties(fixture.new_function.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=1"
    TIMEOUT 30)
