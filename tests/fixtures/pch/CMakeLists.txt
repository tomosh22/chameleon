# Fixture `pch`: a project with a precompiled header (WP-05; the PCH scenarios belong to WP-15).
#
#   pch.h       the precompiled header; target_precompile_headers makes CMake generate the /Yc
#               translation unit (cmake_pch.cxx) and force-include the header into the others
#   logic.cpp   a /Yu translation unit; the patch target (edit value_2.edit: TU change, must hot-reload)
#   main.cpp    a /Yu translation unit with the TICK loop
#   edits/      value_2.edit changes logic.cpp; pch_header.edit changes pch.h (must be rejected: CML2101)
#
# Used by WP-05 to find out how a /Yu translation unit is recompiled when the PCH was built with /Zi.
cml_add_fixture(pch SOURCES main.cpp logic.cpp)
target_precompile_headers(fixture_pch_objs PRIVATE pch.h)

add_test(NAME fixture.pch.runs COMMAND fixture_pch --ticks 3)
set_tests_properties(fixture.pch.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=1 base=100"
    TIMEOUT 30)
