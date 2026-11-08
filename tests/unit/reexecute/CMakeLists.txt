# unit.reexecute (WP-23): the recipe builder, the Exception / Reexecute wire format, the broker's decision (ExceptionService against a
# scripted agent), and - when the fixture is built by this CI cell's compiler - the parameter records of fixture crash_reexecute's PDB.
# Portable: it also runs on Linux with CML_ALLOW_NON_WINDOWS (without the fixture test).
cml_add_unit_test(reexecute
    SOURCES test_recipe.cpp test_messages.cpp test_service.cpp test_proc_info.cpp test_fixture_pdb.cpp
    LIBS cml_core)
if(NOT MSVC)
    target_link_libraries(cml_test_reexecute PRIVATE Threads::Threads)
endif()
if(MSVC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../fixtures/crash_reexecute/CMakeLists.txt")
    target_compile_definitions(cml_test_reexecute PRIVATE
        "CML_FIXTURE_CRASH_REEXECUTE_PDB=\"$<TARGET_PDB_FILE:fixture_crash_reexecute>\""
        "CML_FIXTURE_CRASH_REEXECUTE_EXE=\"$<TARGET_FILE:fixture_crash_reexecute>\"")
    add_dependencies(cml_test_reexecute fixture_crash_reexecute)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_definitions(cml_test_reexecute PRIVATE CML_TEST_COMPILER_IS_CL=1)  # cl.exe, not clang-cl
endif()
set_tests_properties(unit.reexecute PROPERTIES TIMEOUT 300)
