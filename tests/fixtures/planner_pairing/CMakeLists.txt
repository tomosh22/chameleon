# Fixture `planner_pairing` (WP-40, ADR-0043, R-48): one function per bind-planner pairing defect, each in the translation
# unit its edit recompiles (pairing.h says which).
#
#   scope.cpp     item 3: a new file-scope `static int helper(int)` is not the old PpScope::helper
#   guards.cpp    item 2 / the guard rule: a new guarded static gets a new guard, never an old one
#   tsi.cpp       item 7: two same-named block statics under /Zc:threadSafeInit- (cl's `$S1` bitmask guards)
#   internal.cpp  item 11: two same-named block statics of a function with internal linkage
#   swap.cpp      item 5: two lambdas with different call operators swap places: refused
#   sibling.cpp   item 8 (refusal kept): a new guarded static next to an old one in a block: refused
#   edits/        pairing_1 (scope, guards, tsi, internal: reloads), swap_2 (refused), swap_back_3 (undoes swap_2),
#                 sibling_4 (refused); scenario tests/harness/scenarios/planner_pairing.toml.
cml_add_fixture(planner_pairing SOURCES main.cpp scope.cpp guards.cpp tsi.cpp internal.cpp swap.cpp sibling.cpp)
if(MSVC)
    # cl and clang-cl: function-local statics without the thread-safe initialisation (cl guards each block with `$S1`).
    set_source_files_properties(tsi.cpp PROPERTIES COMPILE_OPTIONS "/Zc:threadSafeInit-")
endif()

add_test(NAME fixture.planner_pairing.runs COMMAND fixture_planner_pairing --ticks 3)
set_tests_properties(fixture.planner_pairing.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 member=2 local=10 fresh=1 gt=50003 s0=10003 s1=11003 sm=1 i0=20003 i1=21003 im=1 la=30003 lb=31003 n0=40003 n1=41003 made=8"
    TIMEOUT 30)
