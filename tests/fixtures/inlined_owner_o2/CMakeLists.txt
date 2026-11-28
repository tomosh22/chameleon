# Fixture `inlined_owner_o2` (WP-57, R-59, ADR-0046): built with /O2 /Ob2 in every cell (as planner_pairing_o2), so that
# std::sort and its lambda are really inlined.
#
#   sorted.cpp   io_step(): an own `static Counter` next to a std::sort with a lambda. Until WP-57 the static was refused
#                (CML3207, WP-51) on any edit of the file; now it pairs because io_step()'s code is the old code (the sort
#                loop's instantiation over the closure compared with the old one).
#   edits/       mark_2 (io_mark 1 -> 2), stacked/mark_3 (2 -> 3: io_step compared with the code patch 1 gave it);
#                scenario tests/harness/scenarios/inlined_owner_o2.toml.
if(MSVC)
    # /O2 and /RTC1 cannot be combined (D8016), and the Debug configuration's /Od and /Ob0 must not win over /O2 /Ob2.
    foreach(_cml_flag /RTC1 /Od /Ob0 /JMC)
        string(REPLACE "${_cml_flag}" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    endforeach()
endif()
cml_add_fixture(inlined_owner_o2 SOURCES main.cpp sorted.cpp)
if(MSVC)
    # clang-cl records -fno-inline whenever a /Ob0 is anywhere on the command line (docs/wp/WP-18.md), so it is removed.
    get_target_property(_cml_io_flags fixture_inlined_owner_o2_objs COMPILE_OPTIONS)
    list(FILTER _cml_io_flags EXCLUDE REGEX "Ob0")
    set_target_properties(fixture_inlined_owner_o2_objs PROPERTIES COMPILE_OPTIONS "${_cml_io_flags}")
    target_compile_options(fixture_inlined_owner_o2_objs PRIVATE /O2 /Ob2)
endif()

add_test(NAME fixture.inlined_owner_o2.runs COMMAND fixture_inlined_owner_o2 --ticks 3)
set_tests_properties(fixture.inlined_owner_o2.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 mark=1 count=800003 made=1 top=29"
    TIMEOUT 30)
