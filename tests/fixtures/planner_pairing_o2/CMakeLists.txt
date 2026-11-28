# Fixture `planner_pairing_o2` (WP-40, ADR-0043): built with /O2 /Ob2 in every cell, so that clang-cl really inlines (the
# CI's RelWithDebInfo is /O2 /Ob0 for fixtures, Debug does not optimise). cl builds it the same way, where it is harmless.
#
#   lambdas.cpp  R-48 item 1: three lambdas with a static `sname` each, inlined into o2_step(); clang-cl records the
#                statics in o2_step() itself and no guard at all
#   parts.cpp    R-48 item 4: an internal-linkage array that clang-cl splits into `s_parts.0`, `.1`, `.2`
#   nested.cpp   WP-52 (R-60): a static in a nested lambda, whose guard clang 18 spells with other back-references;
#                WP-56: cl spells its outer closure as a back-reference (R-68), and o2_nest2_step() is the nest inlined
#                all the way (R-66)
#   bitmask.cpp  WP-59 (R-67): /Zc:threadSafeInit-, the two statics of an inline function share clang-cl's `??_B` bitmask
#   edits/       parts_1 (parts.cpp; clang-cl: refused with CML3601, cl: reloads), parts_back_2, mark_3 (lambdas.cpp;
#                the statics keep their values, or the reload is refused), nest_4 (nested.cpp: applied in every cell, both
#                nested statics keep counting), bits_5 (bitmask.cpp: applied under clang-cl, the inline function's statics
#                keep counting; cl: applied or refused); scenario tests/harness/scenarios/planner_pairing_o2.toml.
if(MSVC)
    # /O2 and /RTC1 cannot be combined (D8016), and the Debug configuration's /Od and /Ob0 must not win over /O2 /Ob2 (the
    # same as fixture `optimised`).
    foreach(_cml_flag /RTC1 /Od /Ob0 /JMC)
        string(REPLACE "${_cml_flag}" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    endforeach()
endif()
cml_add_fixture(planner_pairing_o2 SOURCES main.cpp lambdas.cpp parts.cpp nested.cpp bitmask.cpp)
if(MSVC)
    # WP-59: function-local statics without the thread-safe initialisation (clang-cl: -fno-threadsafe-statics).
    set_source_files_properties(bitmask.cpp PROPERTIES COMPILE_OPTIONS "/Zc:threadSafeInit-")
    # clang-cl records -fno-inline whenever a /Ob0 is anywhere on the command line (docs/wp/WP-18.md), so it is removed.
    get_target_property(_cml_o2_flags fixture_planner_pairing_o2_objs COMPILE_OPTIONS)
    list(FILTER _cml_o2_flags EXCLUDE REGEX "Ob0")
    set_target_properties(fixture_planner_pairing_o2_objs PROPERTIES COMPILE_OPTIONS "${_cml_o2_flags}")
    target_compile_options(fixture_planner_pairing_o2_objs PRIVATE /O2 /Ob2)
endif()

add_test(NAME fixture.planner_pairing_o2.runs COMMAND fixture_planner_pairing_o2 --ticks 3)
set_tests_properties(fixture.planner_pairing_o2.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 mark=1 a=100003 b=200006 c=300009 parts=500018 pm=1 made=3 nest=700003 nmade=1 nmark=1 bits=800003 bsec=900003 bmade=2 bmark=1 nest2=600003 n2made=1"
    TIMEOUT 30)
