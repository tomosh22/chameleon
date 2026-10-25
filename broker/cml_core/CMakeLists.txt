configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/cml/core/version_config.h"
    @ONLY)

add_library(cml_core STATIC src/version.cpp)
target_include_directories(cml_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(cml_core PUBLIC cml_util)
cml_apply_warnings(cml_core)
cml_target_core_style(cml_core)
set_target_properties(cml_core PROPERTIES FOLDER "broker")
