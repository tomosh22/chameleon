# Fixture `hot_restart` (WP-21): the process the broker terminates and starts again.
#
#   main.cpp   the loop. `--agent` creates the agent and enables this exe ONLY when CML_RESTART_TOKEN is not set: a process
#              the broker relaunched is enabled again by the broker (ReenableModules), which is what the scenario checks.
#              CML_PRERESTART_HOOK prints `PRERESTART pid=<pid> value=<v>` just before the broker terminates the process.
#              `--label=<text>` is carried in every TICK line: the command line must survive the restart.
#   logic.cpp  restart_value(); the TU the scenario edits (1 -> 2). After the restart the new process must print the
#              edited value: the broker re-applies the session's changes.
#
# Output: `START pid=<pid> restarted=<0|1>` once, `TICK n=<k> value=<v> pid=<pid> restarted=<0|1> label=<text> cwd_ok=<0|1>
# env_ok=<0|1>` every 100 ms (cwd_ok: the working directory is the exe's directory; env_ok: the harness's
# CML_FIXTURE_SIGNAL_FILE variable is still set), `PRERESTART ...` from the hook.
cml_add_fixture(hot_restart SOURCES main.cpp logic.cpp)

add_test(NAME fixture.hot_restart.runs COMMAND fixture_hot_restart --ticks 3 --label=ctest)
set_tests_properties(fixture.hot_restart.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=1 pid=[0-9]+ restarted=0 label=ctest"
    TIMEOUT 30)
