# Fixture `optimised` (WP-18): the only fixture built with optimisation and inlining on (/O2 /Ob2). Every other fixture
# uses the compiler's Debug flags or /Ob0. Editing the small function small_value() must give the warning CML3104
# ("changed but its compiland is optimised; inlined copies elsewhere keep the old code"); `cml_broker check` reports
# the same condition as CML1603.
if(MSVC)
    # /O2 and /RTC1 cannot be combined (D8016), and the Debug configuration's /Od and /Ob0 must not win over /O2 /Ob2.
    foreach(_cml_flag /RTC1 /Od /Ob0 /JMC)
        string(REPLACE "${_cml_flag}" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    endforeach()
endif()
cml_add_fixture(optimised SOURCES main.cpp logic.cpp)
if(MSVC)
    # After cml_add_fixture(): the fixture style's /Ob0 (RelWithDebInfo) comes first, the later /Ob2 wins.
    target_compile_options(fixture_optimised_objs PRIVATE /O2 /Ob2)
endif()

add_test(NAME fixture.optimised.runs COMMAND fixture_optimised --ticks 3)
set_tests_properties(fixture.optimised.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=11 small=1"
    TIMEOUT 30)
