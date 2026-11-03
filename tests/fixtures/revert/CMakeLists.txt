# Fixture `revert` (WP-22): three versions of compute_value() for the patch-revert scenario.
#
#   A (the build)               return 1;
#   B (edits/to_b.edit)         adds the new function revert_helper() and returns 1 + revert_helper()          = 2
#   C (edits/stacked/to_c.edit) applies on top of B: returns 2 + revert_helper()                               = 3
#
# `TICK n=<k> value=<v>`. The scenario reloads B, then C, reverts the last reload (B shows again), reverts all (A shows
# again) and reloads once more (C). revert_helper() is a new function of patch 1; after `revert all` the broker must
# forget it, so the last reload defines it afresh instead of binding to the reverted patch's copy.
# The C edit applies only on top of B, so it lives outside edits/ (unit.link_patch applies every edits/*.edit to the
# pristine sources).
cml_add_fixture(revert SOURCES main.cpp logic.cpp)

add_test(NAME fixture.revert.runs COMMAND fixture_revert --ticks 3)
set_tests_properties(fixture.revert.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=1"
    TIMEOUT 30)
