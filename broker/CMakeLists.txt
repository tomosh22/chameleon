add_subdirectory(cml_core)
add_subdirectory(cml_cli)

if(CML_BUILD_GUI)
    add_subdirectory(cml_gui)
endif()
