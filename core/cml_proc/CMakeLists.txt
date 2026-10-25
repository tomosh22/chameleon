cml_add_core_library(cml_proc
    SOURCES src/proc.cpp
    LIBS cml_util)
if(WIN32)
    # advapi32: tokens and SIDs; shell32: CommandLineToArgvW.
    target_link_libraries(cml_proc PUBLIC advapi32 shell32)
endif()
