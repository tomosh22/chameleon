# Fixture `lambda_statics` (WP-37, ADR-0042): several lambdas in one function, each with a function-local static of
# the same name, in the shape of Godot's SNAME macro (found by the corpus in scene/main/scene_tree.cpp).
#
#   names.h    LS_NAME(id): ([]() -> LsName& { static LsName sname = ls_make_name(id); return sname; })()
#   logic.cpp  ls_step() uses LS_NAME three times; ls_over(int) / ls_over(double) once each (same-named lambdas of
#              two overloads). Each fetch adds to the static's `hits`.
#   edits/     mark_2.edit changes what ls_step() returns (1 -> 2). After the reload the five statics must be the OLD
#              ones: a / b / c / oi / od keep counting from where they were, each in its own range (a wrong pairing
#              would show another lambda's range), and made= stays 5 (no static is initialised again).
cml_add_fixture(lambda_statics SOURCES main.cpp logic.cpp)

add_test(NAME fixture.lambda_statics.runs COMMAND fixture_lambda_statics --ticks 3)
set_tests_properties(fixture.lambda_statics.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 mark=1 a=100003 b=200006 c=300009 oi=400003 od=500003 made=5"
    TIMEOUT 30)
