# WP-26 corpus: raylib 5.5 (zlib) + its example core_2d_camera_platformer as the "game". Configured by
# tests/corpus/run_corpus.ps1 with -DRAYLIB_DIR=<patched checkout> -DCML_COMMON_DIR=<tests/corpus/common>.
#
# raylib's own sources are compiled straight into the executable (the way raylib's Makefile builds a game), not through
# raylib's CMake static library: Chameleon reloads the "source compilands" of a module, and the members of a static
# library are not (the first corpus run showed it: sweep of raylib_game.exe: 6 files checked, the raylib sources were
# not among them; docs/wp/WP-26.md, finding F-1).
cmake_minimum_required(VERSION 3.24)
project(chameleon_corpus_raylib C CXX)

include("${CML_COMMON_DIR}/CorpusFlags.cmake")

# chameleon_corpus.{h,cpp} and Chameleon_API.h were copied into <raylib>/src by the driver.
set(_src "${RAYLIB_DIR}/src")
add_executable(raylib_game
    "${RAYLIB_DIR}/examples/core/core_2d_camera_platformer.c"
    "${_src}/rcore.c" "${_src}/rshapes.c" "${_src}/rtextures.c" "${_src}/rtext.c" "${_src}/rmodels.c"
    "${_src}/raudio.c" "${_src}/utils.c" "${_src}/rglfw.c"
    "${_src}/chameleon_corpus.cpp")
target_include_directories(raylib_game PRIVATE "${_src}" "${_src}/external/glfw/include")
target_compile_definitions(raylib_game PRIVATE PLATFORM_DESKTOP GRAPHICS_API_OPENGL_33 _CRT_SECURE_NO_WARNINGS)
target_link_libraries(raylib_game PRIVATE opengl32 gdi32 winmm shell32 user32)
