# WP-41 corpus: Jolt Physics v5.7.0 (MIT; HelloWorld.cpp is CC0-1.0) built with /arch:AVX2, as static libraries, with
# CMake's unity build on every target. Configured by tests/corpus/run_corpus.ps1 (tests/corpus/jolt/build.ps1) with
# -DJOLT_DIR=<patched checkout> -DCML_COMMON_DIR=<tests/corpus/common>.
#
#   Jolt                 Jolt's own static library target (Build/CMakeLists.txt, unchanged): 153 sources in unity batches
#                        of 8, precompiled header Jolt/Jolt.h, /Zi
#   jolt_corpus_support  static library: chameleon_corpus.cpp (the corpus support code, copied into <jolt> by the driver)
#   jolt_hello           HelloWorld/HelloWorld.cpp (patched: runs until the driver stops it) linked with both libraries
cmake_minimum_required(VERSION 3.24)
project(chameleon_corpus_jolt CXX)

include("${CML_COMMON_DIR}/CorpusFlags.cmake")

# Jolt's headers need C++17 (std::string_view, std::align_val_t). Jolt.cmake sets it only in Jolt's own directory, and cl and
# clang-cl default to C++14, so the two targets of this directory ask for it too.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The three properties of this corpus project (ADR-0043, WP-41). build.ps1 checks each in the build tree.
set(CMAKE_UNITY_BUILD ON)              # every target below and Jolt's; CMake's default batch size (8)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # build.ps1 reads /arch:AVX2 and the unity sources from it
if(MSVC)
    # cl: Jolt's Jolt.cmake adds /arch:AVX2 only for the Visual Studio generator (CMAKE_VS_PLATFORM_NAME); with Ninja it is
    # given here, for every target, and Jolt/Core/Core.h derives JPH_USE_AVX2 (and F16C, LZCNT, TZCNT, FMADD) from __AVX2__.
    # clang-cl: Jolt adds -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c -mfma to its target (PUBLIC); /arch:AVX2 is given as well.
    add_compile_options(/arch:AVX2)
endif()

# Jolt's options (Build/CMakeLists.txt). No /GL (interprocedural optimisation applies to Release only, and is off), no
# /Wall /WX (newer compilers than Jolt's CI), the DLL CRT like the other targets, no GPU compute back ends (no DXC, no
# Vulkan SDK on the runner; the CPU compute back end stays on), no install rules. Static library: JPH_BUILD_SHARED_LIBS off.
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${JOLT_DIR}/Build" jolt)

add_library(jolt_corpus_support STATIC "${JOLT_DIR}/chameleon_corpus.cpp")
target_include_directories(jolt_corpus_support PUBLIC "${JOLT_DIR}")

add_executable(jolt_hello "${JOLT_DIR}/HelloWorld/HelloWorld.cpp")
target_link_libraries(jolt_hello PRIVATE Jolt jolt_corpus_support)
