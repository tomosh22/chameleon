# Placeholder for the Dear ImGui + D3D11 front-end. Only configured with -DCML_BUILD_GUI=ON.
# The real front-end (process list, modules, log, compile output, crash dialog) arrives in a later WP.
add_library(cml_gui STATIC src/gui_placeholder.cpp)
target_include_directories(cml_gui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(cml_gui PUBLIC cml_third_party_imgui)
cml_apply_warnings(cml_gui)
cml_target_exceptions_allowed(cml_gui)
set_target_properties(cml_gui PROPERTIES FOLDER "broker")
