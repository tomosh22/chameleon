# Fixture `lambda_statics` (WP-37, ADR-0042): several lambdas in one function, each with a function-local static of
# the same name, in the shape of Godot's SNAME macro (found by the corpus in scene/main/scene_tree.cpp).
#
#   names.h    LS_NAME(id): ([]() -> LsName& { static LsName sname = ls_make_name(id); return sname; })()
#   logic.cpp  ls_step() uses LS_NAME three times; ls_over(int) / ls_over(double) once each (same-named lambdas of
#              two overloads). Each fetch adds to the static's `hits`.
#              LsTree::flush() / ::timers(): member functions with LS_NAME in block 11 or later (Godot's
#              SceneTree::_flush_scene_change() and ::process_timers(); cl writes such a block in letters, "?L@?").
#              LsTree::options(): two `static const LsList names` in two blocks of one function, no lambda (Godot's
#              SceneTree::get_argument_options()); paired by their place in the function, with the warning CML3570.
#   anon.cpp   a function of an anonymous namespace with two LS_NAME; not edited by anything (evidence only).
#   edits/     mark_2.edit changes what ls_step() returns (1 -> 2). After the reload every static must be the OLD
#              one: each value keeps counting from where it was, in its own range (a wrong pairing would show another
#              static's range), and made= stays 11 (no static is initialised again).
cml_add_fixture(lambda_statics SOURCES main.cpp logic.cpp anon.cpp)

add_test(NAME fixture.lambda_statics.runs COMMAND fixture_lambda_statics --ticks 3)
set_tests_properties(fixture.lambda_statics.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 mark=1 a=100003 b=200006 c=300009 oi=400003 od=500003 tf=600003 tt=700003 nx=800003 ny=900006 p0=1000003 p1=1100003 made=11"
    TIMEOUT 30)
