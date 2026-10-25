# WP-04 benchmark: cml_pdb load time and memory on a large PDB.
#
#   cml_pdb_bench <file.pdb> [--min-size-mb N] [--slack S] [--threads N] [--smoke]
#
# The large PDB is produced by gen_pdb_project.py and built by the `pdb-benchmark` CI job (see
# .github/workflows/ci.yml); that job runs the executable and enforces the budgets. Here the
# executable is built in every cell and ctest runs it in --smoke mode on the fixture PDB so the
# tool itself cannot rot.
add_executable(cml_pdb_bench pdb_bench.cpp)
target_link_libraries(cml_pdb_bench PRIVATE cml_pdb)
if(WIN32)
    target_link_libraries(cml_pdb_bench PRIVATE psapi)
endif()
cml_apply_warnings(cml_pdb_bench)
cml_target_core_style(cml_pdb_bench)
set_target_properties(cml_pdb_bench PROPERTIES FOLDER "tests")

if(MSVC AND TARGET fixture_basic)
    add_test(NAME bench.pdb_smoke COMMAND cml_pdb_bench "$<TARGET_PDB_FILE:fixture_basic>" --smoke)
endif()
