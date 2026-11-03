add_subdirectory(cml_core)
add_subdirectory(cml_cli)
add_subdirectory(cml_ui)  # WP-19: cml_ui_core always, cml_ui.exe with CML_BUILD_GUI

if(CML_BUILD_GUI)
    add_subdirectory(cml_gui)
endif()
