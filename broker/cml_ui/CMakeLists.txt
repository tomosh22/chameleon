# WP-19 broker UI (ADR-0033): cml_ui.exe is a client of the control pipe only.
#
#   cml_ui_core   window-free library: compiler-output parsing, editor command templates, settings, the command
#                 table, the view model and the controller. Portable (built and unit-tested everywhere).
#   cml_ui        the Dear ImGui + Direct3D 11 front-end (Windows, CML_BUILD_GUI=ON), also `cml_ui --self-test`.

add_library(cml_ui_core STATIC
    src/compile_output.cpp
    src/editor.cpp
    src/settings.cpp
    src/commands.cpp
    src/view_model.cpp
    src/controller.cpp)
target_include_directories(cml_ui_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(cml_ui_core PUBLIC cml_ipc cml_util)
if(NOT MSVC)
    find_package(Threads REQUIRED)
    target_link_libraries(cml_ui_core PUBLIC Threads::Threads)
endif()
cml_apply_warnings(cml_ui_core)
cml_target_exceptions_allowed(cml_ui_core)
set_target_properties(cml_ui_core PROPERTIES FOLDER "broker")

if(CML_BUILD_GUI AND WIN32)
    add_executable(cml_ui WIN32
        src/win/main.cpp
        src/win/self_test.cpp
        src/win/broker_link.cpp
        src/win/editor_launch.cpp
        src/win/app.cpp)
    target_link_libraries(cml_ui PRIVATE cml_ui_core cml_proc cml_third_party_imgui
        d3d11 dxgi d3dcompiler dwmapi user32 gdi32 shell32 advapi32 ole32)
    # ImGui and the Win32/D3D11 glue are third-party-shaped code compiled with our warnings, except for the
    # imgui library target itself (relaxed in third_party/CMakeLists.txt).
    cml_apply_warnings(cml_ui)
    cml_target_exceptions_allowed(cml_ui)
    if(MINGW)
        target_link_options(cml_ui PRIVATE -municode)  # wWinMain (mingw-w64 syntax-check builds only)
    endif()
    set_target_properties(cml_ui PROPERTIES FOLDER "broker")
    add_dependencies(cml_ui cml_broker)

    # `cml_ui --self-test`: starts the broker it is given, connects, runs `status`, feeds events through the view
    # model, checks the command table against the advertised capabilities. Creates no window.
    if(CML_BUILD_TESTS)
        add_test(NAME ui.self_test
            COMMAND cml_ui --self-test --broker $<TARGET_FILE:cml_broker>
                    --work-dir "${CMAKE_BINARY_DIR}/test_work/ui_self_test")
        set_tests_properties(ui.self_test PROPERTIES TIMEOUT 180)
    endif()
endif()
