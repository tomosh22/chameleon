# Fixture `guard_bits` (WP-63, R-69, ADR-0047): built with /O2 /Ob2 in every cell (as planner_pairing_o2).
#
#   bits_nts.cpp  /Zc:threadSafeInit-: gb_nts_counter() is inline, keeps `static int calls = 0;` and (edit nts_1) gains a
#                 dynamic static; both share the function's external bitmask `??_B`
#   bits_ts.cpp   thread-safe statics: gb_ts_counter() the same (edit ts_2); the new static's external guard `?$TSS<n>` is
#                 named in calls' block
#   make.cpp      gb_make(), which counts the constructions of the new statics
#   edits/        nts_1 (bits_nts.cpp), ts_2 (bits_ts.cpp): both reload in every cell, the constant-initialised statics keep
#                 counting and each new static is constructed once; scenario tests/harness/scenarios/guard_bits.toml.
if(MSVC)
    # /O2 and /RTC1 cannot be combined (D8016), and the Debug configuration's /Od and /Ob0 must not win over /O2 /Ob2.
    foreach(_cml_flag /RTC1 /Od /Ob0 /JMC)
        string(REPLACE "${_cml_flag}" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    endforeach()
endif()
cml_add_fixture(guard_bits SOURCES main.cpp make.cpp bits_nts.cpp bits_ts.cpp)
if(MSVC)
    # Function-local statics without the thread-safe initialisation (clang-cl: -fno-threadsafe-statics).
    set_source_files_properties(bits_nts.cpp PROPERTIES COMPILE_OPTIONS "/Zc:threadSafeInit-")
    # clang-cl records -fno-inline whenever a /Ob0 is anywhere on the command line (docs/wp/WP-18.md), so it is removed.
    get_target_property(_cml_gb_flags fixture_guard_bits_objs COMPILE_OPTIONS)
    list(FILTER _cml_gb_flags EXCLUDE REGEX "Ob0")
    set_target_properties(fixture_guard_bits_objs PROPERTIES COMPILE_OPTIONS "${_cml_gb_flags}")
    target_compile_options(fixture_guard_bits_objs PRIVATE /O2 /Ob2)
endif()

add_test(NAME fixture.guard_bits.runs COMMAND fixture_guard_bits --ticks 3)
set_tests_properties(fixture.guard_bits.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 nts=100003 ntsx=0 nmark=1 ts=200003 tsx=0 tmark=1 made=0"
    TIMEOUT 30)
