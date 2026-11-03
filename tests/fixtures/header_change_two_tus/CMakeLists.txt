# Fixture `header_change_two_tus` (WP-15): a header-only change must recompile every translation unit that includes it.
#
#   scale.h     `constexpr int kScale = 1;` - holds no code, so clang-cl's PDB does not list it (ADR-0013).
#   shared.h    an inline function returning kScale, included by tu_a.cpp and tu_b.cpp.
#   tu_a.cpp    a_value() = shared_scale() * 10
#   tu_b.cpp    b_value() = shared_scale() * 100
#   edits/      scale_2.edit changes kScale to 2: a=10 b=100 becomes a=20 b=200 (both TUs, no .cpp edited).
cml_add_fixture(header_change_two_tus SOURCES main.cpp tu_a.cpp tu_b.cpp)

add_test(NAME fixture.header_change_two_tus.runs COMMAND fixture_header_change_two_tus --ticks 3)
set_tests_properties(fixture.header_change_two_tus.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 a=10 b=100"
    TIMEOUT 30)
