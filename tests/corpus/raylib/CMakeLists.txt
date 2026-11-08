# WP-26 corpus: raylib 5.5 (zlib) + its example core_2d_camera_platformer as the "game". Configured by
# tests/corpus/run_corpus.ps1 with -DRAYLIB_DIR=<patched checkout> -DCML_COMMON_DIR=<tests/corpus/common>.
cmake_minimum_required(VERSION 3.24)
project(chameleon_corpus_raylib C CXX)

include("${CML_COMMON_DIR}/CorpusFlags.cmake")

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${RAYLIB_DIR}" raylib)

# chameleon_corpus.{h,cpp} and Chameleon_API.h were copied into <raylib>/src by the driver.
add_executable(raylib_game
    "${RAYLIB_DIR}/examples/core/core_2d_camera_platformer.c"
    "${RAYLIB_DIR}/src/chameleon_corpus.cpp")
target_include_directories(raylib_game PRIVATE "${RAYLIB_DIR}/src")
target_link_libraries(raylib_game PRIVATE raylib)
