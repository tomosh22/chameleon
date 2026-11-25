# Fixture `static_overloads` (WP-37, ADR-0042): a header with `static inline` overload sets included by two
# translation units, in the shape of Dear ImGui's imgui_internal.h (corpus finding F-4).
#
#   overloads.h  so_floor(float) / so_floor(const SoVec2&) and so_weight(int) / (long long) / (const char*)
#   tu_a.cpp     a_float() = so_floor(7.9f) = 7,  a_vec() = 35, a_weight() = 11 + 22 + 3 = 36
#   tu_b.cpp     b_float() = so_floor(21.5f) = 21, b_vec() = 82
#   edits/       floor_plus_100.edit changes ONLY so_floor(float): af 7 -> 107 and bf 21 -> 121 in both translation
#                units, av / bv / aw stay (the sibling overloads are not touched), acalls / bcalls keep counting.
cml_add_fixture(static_overloads SOURCES main.cpp tu_a.cpp tu_b.cpp)

add_test(NAME fixture.static_overloads.runs COMMAND fixture_static_overloads --ticks 3)
set_tests_properties(fixture.static_overloads.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 af=7 av=35 aw=36 bf=21 bv=82 acalls=3 bcalls=3"
    TIMEOUT 30)
