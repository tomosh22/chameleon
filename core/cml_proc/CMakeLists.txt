cml_add_core_library(cml_proc
    SOURCES src/proc.cpp
    LIBS cml_util)
if(WIN32)
    # advapi32: tokens and SIDs. Deliberately no shell32: this library is linked into the agent DLL,
    # which is injected into arbitrary hosts and must not pull in more system DLLs than it needs.
    target_link_libraries(cml_proc PUBLIC advapi32)
endif()
