# Fixture `templates`: a header template instantiated in two TUs (COMDAT, selection ANY) and a
# TU-local template with two instantiations whose body the edit changes.
# value = twice(3) + offset(4) + int(offset(1.5)) = 6 + 5 + 2 = 13; after the edit 6 + 104 + 101 = 211.
cml_add_fixture(templates SOURCES main.cpp a.cpp b.cpp)

add_test(NAME fixture.templates.runs COMMAND fixture_templates --ticks 3)
set_tests_properties(fixture.templates.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=13 other=20"
    TIMEOUT 30)
