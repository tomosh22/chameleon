# Fixture `legacy_lambdas` (WP-46, ADR-0043 ruling D1): compiled as C++17 with cl's legacy lambda processor (/Zc:lambda-, the
# default before /std:c++20), which names every closure `<lambda_<32 hex digits>>` in no scope, as in the Jolt corpus
# (F-41.2). The planner pairs such lambdas by that name, and the pipeline proves the pairing by their code (CML3630
# otherwise). clang-cl numbers its lambdas whatever the standard, so in the clang-cl cells this is an ordinary lambda fixture.
#
#   lambdas.cpp  three functions, each calling a lambda that holds a static counter (ids 100000, 200000, 300000)
#   edits/       mark_1 (an edit elsewhere in the file: every lambda unchanged), body_2 (one lambda's body changes),
#                insert_3 (a lambda added in front of the others); scenario tests/harness/scenarios/legacy_lambdas.toml.
cml_add_fixture(legacy_lambdas SOURCES main.cpp lambdas.cpp)
set_target_properties(fixture_legacy_lambdas_objs PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(fixture_legacy_lambdas_objs PRIVATE /Zc:lambda-)
endif()

add_test(NAME fixture.legacy_lambdas.runs COMMAND fixture_legacy_lambdas --ticks 3)
set_tests_properties(fixture.legacy_lambdas.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 mark=1 a=100003 b=200003 c=300003 made=3"
    TIMEOUT 30)
