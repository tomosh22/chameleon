# Fixture `inject` (WP-28): compute_value() returns a constant that the scenario `inject_stages` raises by one for every
# failure-injection stage, and a counter (g_calls, defined in main.cpp) that proves the application kept running.
#
#   edits/value_2.edit              return 1  ->  return 2   (applies to the pristine sources)
#   edits/stacked/value_N.edit      return N-1 -> return N   (N = 3..12; each applies on top of the one before, so they live
#                                   outside edits/: unit.link_patch applies every edits/*.edit to the pristine sources)
cml_add_fixture(inject SOURCES main.cpp logic.cpp)

add_test(NAME fixture.inject.runs COMMAND fixture_inject --ticks 3)
set_tests_properties(fixture.inject.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=1 calls=3"
    TIMEOUT 30)
