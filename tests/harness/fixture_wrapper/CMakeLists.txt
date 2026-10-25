# Standalone wrapper project that builds ONE fixture outside the main tree (used by cmltest on a
# scratch copy of tests/fixtures/<name>, with Ninja or the Visual Studio generator).
#
#   cmake -S tests/harness/fixture_wrapper -B <build> -G Ninja -DCMAKE_BUILD_TYPE=Debug
#         -DCML_REPO_ROOT=<repo> -DCML_FIXTURE_DIR=<copy of tests/fixtures/basic>
#         [-DCML_FIXTURE_ENABLE_C=ON]
#
# It reproduces what the top-level build gives a fixture: C++20, the static CRT, the UNICODE /
# NOMINMAX / WIN32_LEAN_AND_MEAN definitions, one output directory (<build>/bin[/<config>]), the
# fixture helpers (CmlFixture.cmake) and the agent header of THIS repository (the fixture loads
# cml_agent.dll at run time; cmltest copies the prebuilt DLL next to the exe after the build).
cmake_minimum_required(VERSION 3.24)

project(CmlFixtureStandalone LANGUAGES CXX)

if(NOT CML_REPO_ROOT OR NOT EXISTS "${CML_REPO_ROOT}/tests/fixtures/CmlFixture.cmake")
    message(FATAL_ERROR "CML_REPO_ROOT must point at the Chameleon repository (tests/fixtures/CmlFixture.cmake not found)")
endif()
if(NOT CML_FIXTURE_DIR OR NOT EXISTS "${CML_FIXTURE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "CML_FIXTURE_DIR must be a fixture directory containing CMakeLists.txt")
endif()
if(CML_FIXTURE_ENABLE_C)
    enable_language(C)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Used by cml_add_fixture() instead of ${PROJECT_SOURCE_DIR}/agent/include.
set(CML_AGENT_INCLUDE_DIR "${CML_REPO_ROOT}/agent/include")

include("${CML_REPO_ROOT}/cmake/CmlTargets.cmake")
include("${CML_REPO_ROOT}/tests/fixtures/CmlFixture.cmake")

add_subdirectory("${CML_FIXTURE_DIR}" fixture)
