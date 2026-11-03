# unit.ui: the window-free part of the broker UI (WP-19). Portable; also runs on non-Windows hosts.
cml_add_unit_test(ui
    SOURCES
        test_compile_output.cpp
        test_editor.cpp
        test_settings.cpp
        test_commands.cpp
        test_view_model.cpp
        test_controller.cpp
    LIBS cml_ui_core)
