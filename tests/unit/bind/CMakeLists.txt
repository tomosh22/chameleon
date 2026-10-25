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
        test_plan.cpp
        test_chain.cpp
        test_negative.cpp
        test_triples.cpp
        test_golden.cpp
    LIBS cml_core)
target_compile_definitions(cml_test_bind PRIVATE "CML_BIND_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/golden\"")
if(NOT MSVC)
    target_link_libraries(cml_test_bind PRIVATE Threads::Threads)
endif()
