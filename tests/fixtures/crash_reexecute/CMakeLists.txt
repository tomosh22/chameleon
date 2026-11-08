# Fixture `crash_reexecute` (WP-23): crash -> patch -> "Re-execute function".
#
#   main.cpp  the loop (cml_fixture::run, 100 ms). Once the harness creates the signal file (CML_FIXTURE_SIGNAL_FILE, step
#             signal_fixture) it makes ONE call that crashes in step() (stage 1), later ONE call that crashes in opt_step() (stage 3).
#   step.cpp  step(int* p, int k, double scale): one integer pointer, one integer and one double parameter. Compiled WITHOUT
#             optimisation (/Od) in every configuration, so the scenario runs in Debug and RelWithDebInfo alike.
#   opt.cpp   opt_step(int* p, int k): the same crash in a TU compiled WITH optimisation (/O2): re-execute must not be offered.
#   edits/    fix_step.edit, fix_opt.edit: each survives a null `p` (and changes the result so the TICK line shows which body ran).
#
# Output: `TICK n=<k> stage=<s> result=<r> seen_k=<k> seen_scale=<x.xxx> opt=<o>`, `ARMED`.
#
# Flags: the project's Debug flags carry /RTC1 (incompatible with /O2: D8016) and /JMC; they are removed for this directory so one
# fixture can mix /Od and /O2 sources. The per-source options below decide the optimisation level of each TU; the later option wins
# over the configuration's own /Od or /O2 (cl reports D9025, a note-level warning /WX does not escalate; clang-cl takes the last).
if(MSVC)
    foreach(_cml_flag /RTC1 /JMC)
        string(REPLACE "${_cml_flag}" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    endforeach()
endif()
cml_add_fixture(crash_reexecute SOURCES main.cpp step.cpp opt.cpp)
if(MSVC)
    set_source_files_properties(step.cpp PROPERTIES COMPILE_OPTIONS "/Od")
    set_source_files_properties(opt.cpp PROPERTIES COMPILE_OPTIONS "/O2")
endif()

add_test(NAME fixture.crash_reexecute.runs COMMAND fixture_crash_reexecute --ticks 3)
set_tests_properties(fixture.crash_reexecute.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 stage=0 result=509 seen_k=2 seen_scale=0.500 opt=-1"
    TIMEOUT 30)
