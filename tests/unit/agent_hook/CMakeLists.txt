# WP-10: the hook installer.
#
#   unit.agent_hook         instruction-length decoder and strategy planning (portable: byte vectors),
#                           the installer on hand-assembled and compiled functions, thread
#                           suspension and convergence, and InstallHooks through the agent's
#                           dispatcher (Windows)
#   unit.agent_hook_stress  60 s with 16 threads spinning through hooked functions while the main thread
#                           installs, re-points and reverts them (Windows); its own executable so that a
#                           failure of the quick tests never costs a minute, and so that its TIMEOUT
#                           (and with it the hang watchdog, TIMEOUT minus 30 s) fits the 60 s run
#
# The stress test appends its WP10-STRESS lines to ${CMAKE_BINARY_DIR}/test_work/agent_hook/evidence.txt,
# which the CI workflow prints.
#
# WP-38 (ADR-0042): avx/ holds code compiled with /arch:AVX2 and /arch:AVX512. unit.agent_hook_stress hooks functions
# of it whose first instruction is VEX-encoded (line WP38-STRESS-VEX in the same evidence file), and cml_lde_avx_module
# is a whole module of such code for cml_lde_probe (tool.lde_corpus_avx_module) and cml_lde_diff (fuzz.yml).

add_subdirectory(avx)

cml_add_unit_test(agent_hook
    SOURCES test_length_decoder.cpp test_plan.cpp test_user_hooks.cpp
    LIBS cml_agent_core)

if(WIN32)
    # agent/src/operator_new.cpp is what cml_agent.dll links: with it in the test executables every
    # `new` goes through the agent's heap, so heap::set_alloc_forbidden() is really exercised (see
    # alloc_guard.h).
    set(CML_AGENT_OPERATOR_NEW "${PROJECT_SOURCE_DIR}/agent/src/operator_new.cpp")
    target_sources(cml_test_agent_hook PRIVATE test_installer.cpp test_hardening.cpp test_hang_watch.cpp test_dispatch.cpp user_hooks_probe.cpp
                                               ${CML_AGENT_OPERATOR_NEW})
    target_include_directories(cml_test_agent_hook PRIVATE "${PROJECT_SOURCE_DIR}/agent/include")
    # Test functions are located by address: no incremental-link thunks, no identical-code folding.
    target_link_options(cml_test_agent_hook PRIVATE /INCREMENTAL:NO /OPT:NOICF)
    set_tests_properties(unit.agent_hook PROPERTIES TIMEOUT 300)
    # The out-of-process hang watchdog (test_hang_watch.cpp tests it; doctest_main.cpp starts it for the
    # executables that stop the world, because the in-process watchdog is stopped with everything else).
    add_dependencies(cml_test_agent_hook cml_test_hang_watch)
    target_compile_definitions(cml_test_agent_hook PRIVATE "CML_HANG_WATCH_PATH=\"$<TARGET_FILE:cml_test_hang_watch>\"")
    set_property(TEST unit.agent_hook APPEND PROPERTY ENVIRONMENT "CML_TEST_HANG_WATCH_EXE=$<TARGET_FILE:cml_test_hang_watch>")

    cml_add_unit_test(agent_hook_stress SOURCES test_stress.cpp ${CML_AGENT_OPERATOR_NEW} $<TARGET_OBJECTS:cml_test_hook_avx2>
                      LIBS cml_agent_core)
    target_link_options(cml_test_agent_hook_stress PRIVATE /INCREMENTAL:NO /OPT:NOICF)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test_work/agent_hook")
    target_compile_definitions(cml_test_agent_hook_stress PRIVATE
        "CML_TEST_WORK_DIR=\"${CMAKE_BINARY_DIR}/test_work/agent_hook\"")
    set_tests_properties(unit.agent_hook_stress PROPERTIES TIMEOUT 400)
    add_dependencies(cml_test_agent_hook_stress cml_test_hang_watch)
    set_property(TEST unit.agent_hook_stress APPEND PROPERTY ENVIRONMENT "CML_TEST_HANG_WATCH_EXE=$<TARGET_FILE:cml_test_hang_watch>")
endif()

# The decoder against every function of a real module (CI evidence; see docs/wp/WP-10.md). The ctest
# entry runs it on fixture `basic` (MSVC-style builds only: it needs the PDB) and checks it found functions.
add_executable(cml_lde_probe lde_probe.cpp)
target_link_libraries(cml_lde_probe PRIVATE cml_pdb cml_pe cml_agent_core)
cml_apply_warnings(cml_lde_probe)
cml_target_core_style(cml_lde_probe)
set_target_properties(cml_lde_probe PROPERTIES FOLDER "tools")
if(MSVC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/basic/CMakeLists.txt")
    add_dependencies(cml_lde_probe fixture_basic)
    add_test(NAME tool.lde_probe_basic COMMAND cml_lde_probe $<TARGET_FILE:fixture_basic> --quiet)
    set_tests_properties(tool.lde_probe_basic PROPERTIES
        PASS_REGULAR_EXPRESSION "LDE-SUMMARY functions=[1-9][0-9]* decoded=[1-9][0-9]*"
        TIMEOUT 300)
endif()

# WP-38: the module of AVX2 / AVX-512 code. Linked like a module that is enabled for Chameleon (full PDB, no folding,
# nothing discarded, padded entries), compiled per translation unit as avx/CMakeLists.txt says; the "plain" copy of the
# AVX2 code keeps this configuration's own flags (cl /Od in Debug).
if(MSVC)
    add_library(cml_lde_avx2_plain OBJECT avx/lde_avx2_code.cpp)
    target_compile_options(cml_lde_avx2_plain PRIVATE /arch:AVX2)
    target_compile_definitions(cml_lde_avx2_plain PRIVATE CML_AVX_VARIANT=plain)
    cml_apply_warnings(cml_lde_avx2_plain)
    cml_target_core_style(cml_lde_avx2_plain)
    set_target_properties(cml_lde_avx2_plain PROPERTIES FOLDER "tests")

    add_executable(cml_lde_avx_module avx/lde_avx_module_main.cpp $<TARGET_OBJECTS:cml_lde_avx2_plain>
                   $<TARGET_OBJECTS:cml_lde_avx2_optimised> $<TARGET_OBJECTS:cml_lde_avx512_optimised>)
    target_link_options(cml_lde_avx_module PRIVATE /DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN)
    cml_apply_warnings(cml_lde_avx_module)
    cml_target_core_style(cml_lde_avx_module)
    set_target_properties(cml_lde_avx_module PROPERTIES FOLDER "tests")

    # Every function entry of it must get a hook plan: no CML4300 at all, and VEX entries must really be there.
    add_dependencies(cml_lde_probe cml_lde_avx_module)
    add_test(NAME tool.lde_corpus_avx_module
        COMMAND cml_lde_probe $<TARGET_FILE:cml_lde_avx_module> --quiet --fail-on-unknown-opcode --min-functions 60)
    set_tests_properties(tool.lde_corpus_avx_module PROPERTIES
        PASS_REGULAR_EXPRESSION "LDE-OUTCOME functions=[1-9][0-9]* padding=[0-9]+ five_byte=[0-9]+ refused=[0-9]+ refused_vex=0 refused_unknown_opcode=0 refused_other=[0-9]+ vex_entries=[1-9]"
        FAIL_REGULAR_EXPRESSION "cml_lde_probe: [0-9]+ function|LDE-REFUSED"
        TIMEOUT 300)
endif()

# WP-26 (plan section 11, the decoder corpus test): the same probe over every function entry of the other fixtures'
# PDBs, counting the outcomes of plan_hook() (padding / five-byte / refused). Every unknown-opcode refusal (CML4300)
# is a decoder gap and fails the test. That includes VEX/EVEX instructions since WP-38 (ADR-0042): refused_vex must be
# 0 (until then it was a documented limitation; the MSVC STL's vectorised helpers in every fixture were refused). The
# large real-project PDBs are run by .github/workflows/corpus.yml.
if(MSVC)
    foreach(_cml_lde_fx basic templates virtuals statics lambdas optimised tls pch bigobj elision hot_restart new_function)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/${_cml_lde_fx}/CMakeLists.txt")
            add_dependencies(cml_lde_probe fixture_${_cml_lde_fx})
            add_test(NAME tool.lde_corpus_${_cml_lde_fx}
                COMMAND cml_lde_probe $<TARGET_FILE:fixture_${_cml_lde_fx}> --quiet --fail-on-unknown-opcode --min-functions 5)
            set_tests_properties(tool.lde_corpus_${_cml_lde_fx} PROPERTIES
                PASS_REGULAR_EXPRESSION "LDE-OUTCOME functions=[1-9][0-9]* padding=[0-9]+ five_byte=[0-9]+ refused=[0-9]+ refused_vex=0 refused_unknown_opcode=0 "
                FAIL_REGULAR_EXPRESSION "cml_lde_probe: [0-9]+ function|LDE-REFUSED"
                TIMEOUT 300)
        endif()
    endforeach()
endif()
