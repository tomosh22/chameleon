# unit.agent_loader (WP-08): the agent's address-space reservation and patch loader, exercised inside
# this test host process. The "module" patches are placed near is the test executable itself.
#
#   proto/patch_proto.cpp   the prototype patch DLL (no CRT, /FIXED), copied and rebased per test
#   test_loader.cpp         scan/reserve, release-and-load, fix-ups, slots, initializers, unload on failure, dispatcher
#   test_stress.cpp         500 patches (prints WP08-STRESS), failure injection, threads, heap rules
#
# operator new/delete are routed to the agent's private heap exactly as in cml_agent.dll
# (operator_new.cpp), so the tests can assert the heap balance and that apply_fixups does not
# allocate while allocation is forbidden.

add_subdirectory(proto)

cml_add_unit_test(agent_loader
    SOURCES
        loader_test_support.cpp
        test_loader.cpp
        test_stress.cpp
        "${PROJECT_SOURCE_DIR}/agent/src/operator_new.cpp"
    LIBS cml_agent_core cml_pe)
target_compile_definitions(cml_test_agent_loader PRIVATE
    "CML_TEST_PATCH_PROTO=\"$<TARGET_FILE:cml_test_patch_proto>\""
    "CML_TEST_DUMMY_MODULE=\"$<TARGET_FILE:cml_test_dummy_module>\"")
add_dependencies(cml_test_agent_loader cml_test_patch_proto cml_test_dummy_module)
set_tests_properties(unit.agent_loader PROPERTIES TIMEOUT 300)
