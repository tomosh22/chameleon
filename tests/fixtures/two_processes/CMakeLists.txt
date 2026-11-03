# Fixture `two_processes` (WP-16): the same DLL in two processes (a "server" and a "client").
#
#   fixture_two_processes_lib.dll      lib.cpp      tp_value() (edit: 7 -> 70), with a per-process call counter
#   fixture_two_processes.exe          server.cpp   TICK role=server ...   (the harness's primary fixture exe)
#   fixture_two_processes_client.exe   client.cpp   TICK role=client ...   (started by a second `launch` step)
#
# Both exes load the DLL from their own directory, enable it (and themselves), and print the DLL's value and its
# call counter. One edit of lib.cpp must be compiled once and linked + loaded once per process.
cml_add_fixture_dll(fixture_two_processes_lib SOURCES lib.cpp)
cml_add_fixture(two_processes SOURCES server.cpp)
cml_add_fixture(two_processes_client SOURCES client.cpp)
foreach(_cml_tp fixture_two_processes fixture_two_processes_client)
    target_link_libraries(${_cml_tp} PRIVATE fixture_two_processes_lib)
endforeach()
target_include_directories(fixture_two_processes_objs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(fixture_two_processes_client_objs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(fixture_two_processes_lib PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_test(NAME fixture.two_processes.server.runs COMMAND fixture_two_processes --ticks 3)
set_tests_properties(fixture.two_processes.server.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 role=server value=7 calls=3"
    TIMEOUT 30)
add_test(NAME fixture.two_processes.client.runs COMMAND fixture_two_processes_client --ticks 3)
set_tests_properties(fixture.two_processes.client.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 role=client value=7 calls=6"
    TIMEOUT 30)
