# unit.bind: the WP-07 planner (broker/cml_core: bind_plan, bind_names, symbol_registry).
#   test_names.cpp     MSVC-name reader and generated-symbol recognisers
#   test_plan.cpp      every row of the plan 4.5 table on synthetic objects (WP-06 writer) and PDBs (WP-04 builder)
#   test_chain.cpp     every step of the resolution chain, statics bridging (ADR-0010)
#   test_negative.cpp  CML3101 / CML3102 / CML32xx diagnostics
#   test_triples.cpp   (rva, kind, value) generation, layout from a patch PDB, binding.obj hand-off
#   test_golden.cpp    golden JSON plans of synthetic scenarios (golden/*.json) and trace logging
cml_add_unit_test(bind
    SOURCES
        test_names.cpp
        test_c_statics.cpp
        test_plan.cpp
        test_chain.cpp
        test_negative.cpp
        test_triples.cpp
        test_golden.cpp
        test_scope.cpp
        test_thread_local.cpp
        test_thread_local_review.cpp
        test_types.cpp  # WP-37: overloads by signature, lambda-local statics by closure
        test_imgrel.cpp  # WP-36: image-relative references to old data (ADR-0042)
        test_lambda_code.cpp  # WP-40: lambdas told apart by the code of their call operators (rule (d))
        test_pairing.cpp  # WP-40: pairing defects (R-48): inlined lambdas, guards and their statics, scopes, split aggregates
        test_bitmask_code.cpp  # WP-40: a `$S<n>` bitmask kept only for an unchanged function (ADR-0043 ruling)
        test_hashed_code.cpp  # WP-46: lambdas cl names by a hash, proven by the code of their functions (CML3630)
        test_inlined_lambdas.cpp  # WP-51: an own static never takes an inlined lambda's record (R-54, CML3207)
        test_inlined_mirror.cpp  # WP-53: a lambda's static never takes its caller's deleted static (R-61, CML3207; wp53/)
        test_bitmask_inline.cpp  # WP-59: clang's `??_B` bitmask of an inline function's statics is a guard (R-67; wp59/)
        test_inlined_owners.cpp  # WP-57: an own static next to inlined closure code, proven by its function's code (R-59; wp57/)
    LIBS cml_core)
target_compile_definitions(cml_test_bind PRIVATE "CML_BIND_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/golden\""
                                               "CML_BIND_WP53_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/wp53\""
                                               "CML_BIND_WP57_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/wp57\""
                                               "CML_BIND_WP59_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/wp59\"")
if(NOT MSVC)
    target_link_libraries(cml_test_bind PRIVATE Threads::Threads)
endif()

# unit.bind_fixtures (MSVC only): the M1 fixtures. Each fixture's edited TU (edited/) is planned against
# the ORIGINAL fixture module's PDB; golden/fixtures/<name>.json are the compiler-independent expectations.
if(MSVC AND EXISTS "${PROJECT_SOURCE_DIR}/tests/fixtures/basic/CMakeLists.txt")
    add_subdirectory(edited)
    get_property(_cml_bind_lines GLOBAL PROPERTY CML_BIND_FIXTURE_LINES)
    get_property(_cml_bind_targets GLOBAL PROPERTY CML_BIND_FIXTURE_TARGETS)
    string(REPLACE ";" "\n" _cml_bind_content "${_cml_bind_lines}")
    set(_cml_bind_paths "${CMAKE_CURRENT_BINARY_DIR}/bind_fixtures_$<CONFIG>.txt")
    file(GENERATE OUTPUT "${_cml_bind_paths}" CONTENT "config=$<CONFIG>\n${_cml_bind_content}\n")
    cml_add_unit_test(bind_fixtures
        SOURCES test_fixtures.cpp
        LIBS cml_core)
    target_compile_definitions(cml_test_bind_fixtures PRIVATE
        "CML_BIND_PATHS_FILE=\"${_cml_bind_paths}\""
        "CML_BIND_FIXTURE_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/golden/fixtures\""
        "CML_BIND_WORK_DIR=\"${CMAKE_BINARY_DIR}/test_work/bindplan\"")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_definitions(cml_test_bind_fixtures PRIVATE CML_BIND_COMPILER_IS_CL=1)  # cl.exe, not clang-cl
    endif()
    add_dependencies(cml_test_bind_fixtures ${_cml_bind_targets})
    set_tests_properties(unit.bind_fixtures PROPERTIES TIMEOUT 600)
endif()
