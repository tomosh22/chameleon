# Fixture `elision` (WP-25): one translation unit (elision.cpp) with several functions, none inlined: two instantiations of a
# TU-local template, a function that returns a string literal, a small function (`elision_inner`) and one that calls it
# (`elision_outer`), and `elision_base`. The edits change ONE function each, so the reload reports one hook:
#   edits/base_20.edit          elision_base 10 -> 20
#   edits/stacked/inner_7.edit  elision_inner 5 -> 7 (applies on top of base_20; the hook of elision_base must not be repeated)
#   edits/stacked/comment.edit  a comment only (compiles to identical code: "no functional change")
# TICK n=<k> base=10 tmpl=18 text=alpha chain=6
cml_add_fixture(elision SOURCES main.cpp elision.cpp)

add_test(NAME fixture.elision.runs COMMAND fixture_elision --ticks 3)
set_tests_properties(fixture.elision.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 base=10 tmpl=18 text=alpha chain=6"
    TIMEOUT 30)
