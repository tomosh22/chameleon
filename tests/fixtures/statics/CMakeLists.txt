# Fixture `statics`: file-static data, a function-local static with a guard, static member data and
# const data in .rdata. The edit changes statics_value()'s formula; all statics must stay bound to
# the running process's storage, so `calls=` keeps counting instead of restarting.
cml_add_fixture(statics SOURCES main.cpp statics.cpp)

add_test(NAME fixture.statics.runs COMMAND fixture_statics --ticks 3)
set_tests_properties(fixture.statics.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=3 calls=3 total=[0-9]+"
    TIMEOUT 30)
