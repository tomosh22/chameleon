# Fixture `c_statics` (WP-26c): a C program whose statics are zero-initialised structs holding a pointer: one defined in
# an #included .c file, one in the edited translation unit, one function-local. The TICK loop reads each through its
# pointer. The edit changes cs_read(); every static must stay bound to the running process's storage (a patch with a
# zero-filled copy would crash on the null pointer, or restart the counters).
cml_add_fixture(c_statics C SOURCES main.c c_statics.c)

add_test(NAME fixture.c_statics.runs COMMAND fixture_c_statics --ticks 3)
set_tests_properties(fixture.c_statics.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 inc=3 main=3 local=3"
    TIMEOUT 30)
