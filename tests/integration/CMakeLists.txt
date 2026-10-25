# Integration tests that run real processes: a headless cml_broker, a fixture with the agent, and
# the driver cml_it_agent that starts them, bounds every wait, cleans up through a kill-on-close job
# object and checks both sides' logs. Pipe names are unique per run, so these tests are safe to run
# in parallel with each other and with reruns.

add_executable(cml_it_agent agent_driver.cpp)
target_link_libraries(cml_it_agent PRIVATE cml_ipc cml_proc)
cml_apply_warnings(cml_it_agent)
cml_target_exceptions_allowed(cml_it_agent)
set_target_properties(cml_it_agent PROPERTIES FOLDER "tests")
# The driver starts these by path; make sure they are built before any test runs.
add_dependencies(cml_it_agent cml_broker cml_agent fixture_basic fixture_c_only)

set(_cml_it_work "${CMAKE_BINARY_DIR}/test_work/integration")

# cml_add_agent_it(<test name> <fixture target> <module file name> <mode> [TICKS n])
function(cml_add_agent_it test fixture module mode)
    cmake_parse_arguments(I "" "TICKS" "" ${ARGN})
    if(NOT I_TICKS)
        set(I_TICKS 20)
    endif()
    add_test(NAME ${test}
        COMMAND cml_it_agent
            --broker $<TARGET_FILE:cml_broker>
            --fixture $<TARGET_FILE:${fixture}>
            --module-name ${module}
            --mode ${mode}
            --workdir "${_cml_it_work}"
            --ticks ${I_TICKS}
            --timeout-ms 90000)
    set_tests_properties(${test} PROPERTIES TIMEOUT 240)
endfunction()

# Acceptance (WP-11): the fixture registers with a headless broker and enables its module.
cml_add_agent_it(integration.agent_enable_module fixture_basic fixture_basic.exe prestart)
# The broker is not running: the agent starts cml_broker.exe --headless by itself.
cml_add_agent_it(integration.agent_autostarts_broker fixture_basic fixture_basic.exe autostart)
# No broker and none can be started: the application runs normally.
cml_add_agent_it(integration.agent_without_broker_runs fixture_basic fixture_basic.exe nobroker TICKS 8)
# The C99 fixture through the C API, against a headless broker (and with auto-start).
cml_add_agent_it(integration.c_only_agent_enable_module fixture_c_only fixture_c_only.exe prestart)
cml_add_agent_it(integration.c_only_agent_autostarts_broker fixture_c_only fixture_c_only.exe autostart)
