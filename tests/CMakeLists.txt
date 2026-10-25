add_subdirectory(unit)

# Later work packages add these directories (each with its own CMakeLists.txt):
#   tests/fixtures  small console apps, one per feature (see docs/IMPLEMENTATION_PLAN.md section 10)
#   tests/harness   cmltest.exe, which builds fixtures and drives the broker
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/CMakeLists.txt")
    add_subdirectory(fixtures)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/harness/CMakeLists.txt")
    add_subdirectory(harness)
endif()

# WP-04: cml_pdb_bench (load time / memory benchmark; the large-PDB run is the `pdb-benchmark` CI job).
# Added after fixtures/ so that it can refer to the fixture targets.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt")
    add_subdirectory(bench)
endif()
