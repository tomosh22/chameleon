# The agent.
#
#   cml_agent_core  static library with all agent logic (agent thread, request table, SyncPoint
#                   queue, private heap, broker launch). Separate from the DLL so that unit tests
#                   can link the pieces directly.
#   cml_agent       cml_agent.dll: DllMain, the exported C functions and the operator new/delete
#                   replacement that routes the DLL's C++ allocations to the private heap.
#
# Agent style: static CRT, no exceptions, no RTTI, no third-party code (see cmake/CmlTargets.cmake).
# Applications never link the agent; Chameleon_API.h loads the DLL at run time.

set(CML_AGENT_PORTABLE_SOURCES
    src/sync_queue.cpp
    src/module_diff.cpp)
set(CML_AGENT_WINDOWS_SOURCES
    src/agent.cpp
    src/agent_log.cpp
    src/broker_launch.cpp
    src/heap.cpp
    # WP-08: address-space reservation and patch loading
    src/loader/image_writer.cpp
    src/loader/patch_loader.cpp)

add_library(cml_agent_core STATIC ${CML_AGENT_PORTABLE_SOURCES} ${CML_AGENT_WINDOWS_SOURCES})
target_include_directories(cml_agent_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Chameleon_API.h only provides the shared types inside the agent.
target_compile_definitions(cml_agent_core PRIVATE CML_AGENT_BUILD)
target_link_libraries(cml_agent_core PUBLIC cml_ipc cml_proc cml_util)
cml_apply_warnings(cml_agent_core)
cml_target_agent_style(cml_agent_core)
set_target_properties(cml_agent_core PROPERTIES FOLDER "agent")

if(WIN32)
    add_library(cml_agent SHARED src/dllmain.cpp src/exports.cpp src/operator_new.cpp)
    target_include_directories(cml_agent PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(cml_agent PRIVATE CML_AGENT_BUILD)
    target_link_libraries(cml_agent PRIVATE cml_agent_core)
    cml_apply_warnings(cml_agent)
    cml_target_agent_style(cml_agent)
    set_target_properties(cml_agent PROPERTIES FOLDER "agent")
endif()
