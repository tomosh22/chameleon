# Fixture `basic`: free function returning a constant; the canonical smoke test.
#
#   logic.cpp   compute_value(); the TU the M0 spike (WP-03) and the `basic` scenario patch.
#               It uses exactly two externals from main.cpp: data `g_tick`, function `fixture_note`.
#   extras.cpp  object-format coverage for reader tests (WP-02): COMDAT functions, static data,
#               string literals, .pdata/.xdata with a C++ EH handler, .CRT$XCU dynamic
#               initialisers, weak externals (/alternatename, and clang `weak`), __imp_ calls.
#   edits/      unified diffs applied by the harness.
cml_add_fixture(basic SOURCES main.cpp logic.cpp extras.cpp)

add_test(NAME fixture.basic.runs COMMAND fixture_basic --ticks 3)
set_tests_properties(fixture.basic.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=1 extras=[0-9]+"
    TIMEOUT 30)
