# WP-26 corpus: Dear ImGui v1.91.9b (MIT), examples/example_win32_directx11 with its own main loop. Configured by
# tests/corpus/run_corpus.ps1 with -DIMGUI_DIR=<patched checkout> -DCML_COMMON_DIR=<tests/corpus/common>.
cmake_minimum_required(VERSION 3.24)
project(chameleon_corpus_imgui CXX)

include("${CML_COMMON_DIR}/CorpusFlags.cmake")

# chameleon_corpus.{h,cpp} and Chameleon_API.h were copied into <imgui> by the driver.
add_executable(imgui_dx11
    "${IMGUI_DIR}/examples/example_win32_directx11/main.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/chameleon_corpus.cpp")
target_include_directories(imgui_dx11 PRIVATE "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_compile_definitions(imgui_dx11 PRIVATE UNICODE _UNICODE)
target_compile_options(imgui_dx11 PRIVATE /utf-8)
target_link_libraries(imgui_dx11 PRIVATE d3d11 d3dcompiler dxgi)
