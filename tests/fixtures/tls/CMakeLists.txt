# Fixture `tls` (WP-24): thread_local read/write. The main thread and one worker thread each own a thread_local counter at
# namespace scope (t_counter) and a function-local static thread_local (calls); every frame both add the step to both.
#
#   main.cpp   the loop (cml_fixture::run) and the worker thread (one step per 100 ms). Never patched.
#   logic.cpp  tls_step(): the only TU the scenario patches. Owns both thread_locals.
#
# Output: `TICK n=<k> step=<s> main_ns=<v> main_fn=<v> work_ns=<v> work_fn=<v>` (ns: namespace-scope, fn: function-local;
# the worker's values are published by the worker thread, 0 before its first step).
#
# edits/step_5.edit      changes the step 1 -> 5 (existing TLS: bound to the old storage, the values continue).
# edits/stacked/         applied on top of step_5 by the scenario only (the link tests apply every edits/*.edit to pristine
#                        sources): add_tls.edit adds a NEW thread_local, which the planner rejects (CML3400).
cml_add_fixture(tls SOURCES main.cpp logic.cpp)

add_test(NAME fixture.tls.runs COMMAND fixture_tls --ticks 4)
set_tests_properties(fixture.tls.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=3 step=1 main_ns=4 main_fn=4 work_ns=[0-9]+ work_fn=[0-9]+"
    TIMEOUT 30)
