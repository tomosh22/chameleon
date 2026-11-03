# Fixture `crash` (WP-20): crash -> patch -> continue.
#
#   main.cpp   the loop (cml_fixture::run, 100 ms). Owns a page, g_cell. Once the harness creates the signal file
#              (CML_FIXTURE_SIGNAL_FILE, step signal_fixture) the fixture "arms" itself: it makes the page inaccessible, which
#              is the bug. From then on cell_store() faults on every frame. A helper thread calls maintain() every 10 ms.
#              `--leave`: after a frame whose cell_store() returned 0 the application recovers by making the page accessible
#              again (so the scenario that answers "leave function" faults exactly once).
#   logic.cpp  cell_store() (where the fault happens) and maintain() (what the edit fixes). The only TU the scenarios patch.
#
# Output: `TICK n=<k> cell=<v> armed=<0|1>` (v: the value read back from the page; 0 when the fault was left), `ARMED`
# when the page was made inaccessible, `REPAIRED` once the patched maintain() ran.
#
# Why the fix is in maintain() and Continue is safe: Continue re-executes the faulting instruction with the register values it
# had at the fault. The pointer in the register is the page's address, which is fine; what is wrong is the page. maintain()
# runs on another thread while the main thread is held in the crash handler, and the patched version makes the page accessible,
# so the instruction succeeds when it runs again. (A register that holds null would stay null: Continue cannot repair that.
# See docs/wp/WP-20.md, "Continue: the rule".)
cml_add_fixture(crash SOURCES main.cpp logic.cpp)

add_test(NAME fixture.crash.runs COMMAND fixture_crash --ticks 3)
set_tests_properties(fixture.crash.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 cell=102 armed=0"
    TIMEOUT 30)
