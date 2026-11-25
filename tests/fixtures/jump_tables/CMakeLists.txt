# Fixture `jump_tables` (WP-49, R-46): one translation unit (jump_tables.cpp) with two functions that each hold a dense
# switch (a jump table: in the function's own code under cl, in .rdata under clang-cl) and one function without (jt_value).
# Every reload recompiles the whole unit:
#   edits/value_2.edit               jt_value 1 -> 2: under cl one hook (both switches equal the running code, table included)
#   edits/stacked/swap_cases.edit    `case 1:` and `case 2:` of jt_dense exchange their bodies (d1 50 -> 3, d2 3 -> 50): under
#                                    cl /Od the code bytes stay the same and only the jump table differs; it must be hooked
#   edits/stacked/value_3.edit       jt_value 2 -> 3: jt_dense is compared with the record of the patch that hooked it
# TICK n=<k> d1=50 d2=3 o=95 v=1
cml_add_fixture(jump_tables SOURCES main.cpp jump_tables.cpp)

add_test(NAME fixture.jump_tables.runs COMMAND fixture_jump_tables --ticks 3)
set_tests_properties(fixture.jump_tables.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 d1=50 d2=3 o=95 v=1"
    TIMEOUT 30)
