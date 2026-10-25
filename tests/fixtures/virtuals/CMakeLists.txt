# Fixture `virtuals`: objects created at startup keep working when a virtual override's body
# changes. Square(3) + Rect(2,5): value=19; after the edit Square::area doubles: value=28.
cml_add_fixture(virtuals SOURCES main.cpp shapes.cpp)

add_test(NAME fixture.virtuals.runs COMMAND fixture_virtuals --ticks 3)
set_tests_properties(fixture.virtuals.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=19 shapes=2"
    TIMEOUT 30)
