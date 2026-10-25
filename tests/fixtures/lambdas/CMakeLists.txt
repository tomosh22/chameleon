# Fixture `lambdas`: a capturing lambda stored in a std::function before any patch and called
# every frame. value = (n % 3) + 5; after the edit (n % 3) + 50.
cml_add_fixture(lambdas SOURCES main.cpp lambdas.cpp)

add_test(NAME fixture.lambdas.runs COMMAND fixture_lambdas --ticks 3)
set_tests_properties(fixture.lambdas.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=7"
    TIMEOUT 30)
