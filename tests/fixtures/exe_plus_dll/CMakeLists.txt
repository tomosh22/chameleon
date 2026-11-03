# Fixture `exe_plus_dll` (WP-16): one process, two modules the broker patches independently.
#
#   fixture_exe_plus_dll_lib.dll   lib.cpp      lib_scale()  called by the exe through its import (__imp_lib_scale slot)
#                                               lib_apply()  calls back into the exe through a function pointer
#                                               lib_offset() unchanged DLL function the patched exe code imports
#   fixture_exe_plus_dll.exe       exe_logic.cpp exe_callback(), exe_import_user() (calls lib_offset through __imp_)
#                                  main.cpp      TICK loop; enables the exe and the DLL
#
# Edits: dll_scale_30.edit changes the DLL (the exe's call reaches the new code through the hook on the old entry),
# exe_changes.edit changes the exe (the DLL's call through the callback pointer reaches it, and the patched exe code
# reads the DLL's import slot). Scenario `exe_plus_dll` applies both and reloads once.
cml_add_fixture_dll(fixture_exe_plus_dll_lib SOURCES lib.cpp)
cml_add_fixture(exe_plus_dll SOURCES main.cpp exe_logic.cpp)
target_link_libraries(fixture_exe_plus_dll PRIVATE fixture_exe_plus_dll_lib)
target_include_directories(fixture_exe_plus_dll_objs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# The DLL's objects are compiled with the same include directories as the exe's.
target_include_directories(fixture_exe_plus_dll_lib PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME fixture.exe_plus_dll.runs COMMAND fixture_exe_plus_dll --ticks 3)
set_tests_properties(fixture.exe_plus_dll.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 dll=6 cb=6 imp=10"
    TIMEOUT 30)
