# unit.coff: cml_coff reader tests.
#   test_coff_synthetic.cpp   hand-built objects (regular + bigobj), malformed-input cases
#   test_coff_fixture.cpp     acceptance tests on the objects cl / clang-cl produce from fixtures
#   test_coff_roundtrip.cpp   independent re-parse + the WP-06 writer round-trip hook
#
# Fixture output paths reach the tests through a text file written by file(GENERATE) (one
# `key=value` per line, objects `;`-separated), whose location is the CML_TEST_PATHS_FILE macro.

# tests/fixtures/basic/extras.cpp compiled at /Od in every configuration. An optimising compiler
# folds most of the constructs extras.cpp exists to produce (clang-cl /O2 /Ob0 drops the inline and
# template COMDATs, static data, string literal and EH personality), so the strict object-format
# assertions run on this object while the fixture's own objects are checked for what survives.
# Same source, includes and compile flags as the fixture otherwise (/Gy /hotpatch, exceptions on).
add_library(coff_cov_extras_objs OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/basic/extras.cpp")
cml_target_exceptions_allowed(coff_cov_extras_objs)
if(MSVC)
    # /Ob0 as well: CMake's RelWithDebInfo flags carry /Ob1, which still inlines at /Od under cl.
    target_compile_options(coff_cov_extras_objs PRIVATE /Od /Ob0 /Gy /hotpatch)
else()
    target_compile_options(coff_cov_extras_objs PRIVATE -O0)
endif()
set_target_properties(coff_cov_extras_objs PROPERTIES FOLDER "tests")

# cml_define_fixture_paths(<test target>): shared with tests/unit/pe.
function(cml_define_fixture_paths target)
    set(paths_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_fixture_paths_$<CONFIG>.txt")
    if(MSVC)
        set(basic_pdb "$<TARGET_PDB_FILE:fixture_basic>")
    else()
        set(basic_pdb "")  # no PDBs off Windows (only reachable with CML_ALLOW_NON_WINDOWS)
    endif()
    file(GENERATE OUTPUT "${paths_file}" CONTENT
"config=$<CONFIG>
basic_objs=$<TARGET_OBJECTS:fixture_basic_objs>
cov_extras_objs=$<TARGET_OBJECTS:coff_cov_extras_objs>
bigobj_objs=$<TARGET_OBJECTS:fixture_bigobj_objs>
basic_exe=$<TARGET_FILE:fixture_basic>
basic_pdb=${basic_pdb}
bigobj_exe=$<TARGET_FILE:fixture_bigobj>
")
    target_compile_definitions(${target} PRIVATE "CML_TEST_PATHS_FILE=\"${paths_file}\"")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../support")
    add_dependencies(${target} fixture_basic fixture_basic_objs fixture_bigobj fixture_bigobj_objs coff_cov_extras_objs)
endfunction()

cml_add_unit_test(coff
    SOURCES
        test_coff_synthetic.cpp
        test_coff_fixture.cpp
        test_coff_roundtrip.cpp
    LIBS cml_coff)
cml_define_fixture_paths(cml_test_coff)
