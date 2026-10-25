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
        test_coff_writer.cpp
    LIBS cml_coff)
cml_define_fixture_paths(cml_test_coff)

# WP-06 acceptance: anchors and binding.obj through the real linkers (link.exe and lld-link).
# Needs both linkers (CI cells provide them); CML_TEST_LINKERS=lld-link restricts local runs. Writes
# <build>/test_work/coff_link/evidence.txt, which CI prints.
cml_add_unit_test(coff_link
    SOURCES test_coff_link.cpp
    LIBS cml_coff cml_pe cml_pdb)
cml_define_fixture_paths(cml_test_coff_link)
target_compile_definitions(cml_test_coff_link PRIVATE "CML_TEST_WORK_DIR=\"${CMAKE_BINARY_DIR}/test_work/coff_link\"")
set_tests_properties(unit.coff_link PROPERTIES TIMEOUT 1800)
# Where link.exe is, independent of PATH (the Visual Studio generator job has no developer
# environment): CMAKE_LINKER when it is link.exe, else the directory of cl.exe.
set(_cml_msvc_link "")
if(CMAKE_LINKER MATCHES "[/\\\\]link\\.exe$")
    set(_cml_msvc_link "${CMAKE_LINKER}")
else()
    get_filename_component(_cml_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(EXISTS "${_cml_cxx_dir}/link.exe")
        set(_cml_msvc_link "${_cml_cxx_dir}/link.exe")
    endif()
endif()
if(_cml_msvc_link)
    target_compile_definitions(cml_test_coff_link PRIVATE "CML_TEST_MSVC_LINK=\"${_cml_msvc_link}\"")
endif()
