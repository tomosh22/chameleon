# cml_ipc: wire format (messages.cpp, portable) and overlapped named-pipe transport (pipe.cpp,
# Windows only). Linked into the agent, so core style: no exceptions, no RTTI, no third-party code.
cml_add_core_library(cml_ipc
    SOURCES
        src/messages.cpp
        src/pipe.cpp
    LIBS cml_util cml_proc)
