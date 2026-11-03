# unit.agent_exception (WP-20): the agent's first-chance exception handler (crash -> patch -> continue). Every scenario faults on
# purpose and runs in a child process (the executable starts itself with --test-case=exc_child); see test_exception_gate.cpp.
cml_add_unit_test(agent_exception
    SOURCES test_exception_gate.cpp
    LIBS cml_agent_core)
set_tests_properties(unit.agent_exception PROPERTIES TIMEOUT 300)
