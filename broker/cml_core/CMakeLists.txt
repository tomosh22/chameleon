configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/cml/core/version_config.h"
    @ONLY)

# session.cpp is portable (unit-testable without pipes); connection_server.cpp is Windows-only.
# WP-13: process_registry.cpp and the handler half of control_server.cpp are portable too.
add_library(cml_core STATIC
    src/version.cpp
    src/session.cpp
    src/connection_server.cpp
    src/process_registry.cpp
    src/control_server.cpp
    # WP-07: bind planner (portable; consumes cml_coff and cml_pdb)
    src/bind_names.cpp
    src/bind_plan.cpp
    src/bind_fixups.cpp
    src/bind_json.cpp
    src/symbol_registry.cpp)
target_include_directories(cml_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(cml_core PUBLIC cml_util cml_ipc cml_coff cml_pdb)
cml_apply_warnings(cml_core)
cml_target_core_style(cml_core)
set_target_properties(cml_core PROPERTIES FOLDER "broker")
