# cml_ipc: wire format (messages.cpp, portable), overlapped named-pipe transport (pipe.cpp,
# Windows only) and the JSON control protocol (json.cpp, control.cpp: the codec is portable, the
# pipe channel/client are Windows only, WP-13). Linked into the agent, so core style: no
# exceptions, no RTTI, no third-party code.
cml_add_core_library(cml_ipc
    SOURCES
        src/messages.cpp
        src/pipe.cpp
        src/json.cpp
        src/control.cpp
    LIBS cml_util cml_proc)
