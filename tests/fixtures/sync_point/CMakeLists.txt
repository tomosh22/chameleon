# Fixture `sync_point` (WP-17): synchronised mode and the application hooks.
#
#   main.cpp   the loop. `--agent` creates a SYNCHRONISED agent (`--immediate`: a default agent), enables
#              this exe, calls SyncPoint() once per tick on the main thread, brackets the call with an
#              "inside SyncPoint" flag, and runs a watcher thread that records WHERE the new value of
#              compute_value() was first seen. `--hold-sync` withholds SyncPoint() until the file named by
#              CML_FIXTURE_SIGNAL_FILE exists (the harness step `signal_fixture` creates it).
#   hooks.cpp  CML_PREPATCH_HOOK / CML_POSTPATCH_HOOK / CML_COMPILE_*_HOOK functions that print which thread
#              they run on and whether that thread is inside SyncPoint(). Never patched.
#   logic.cpp  compute_value(); the TU the scenarios edit (value 1 -> 42).
#
# Output: `TICK n=<k> value=<v> change=<0|1|2>` (change: 0 = not changed yet, 1 = the change was first seen
# while the main thread was inside SyncPoint(), 2 = seen outside it) and `HOOK <what> ...` lines.
cml_add_fixture(sync_point SOURCES main.cpp hooks.cpp logic.cpp)

add_test(NAME fixture.sync_point.runs COMMAND fixture_sync_point --ticks 3)
set_tests_properties(fixture.sync_point.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=1 change=0"
    TIMEOUT 30)
