cmake_minimum_required(VERSION 3.24)

project(Chameleon
    VERSION 0.1.0
    DESCRIPTION "Native C/C++ hot-reload tool for Windows x64"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(CML_ALLOW_NON_WINDOWS "Allow configuring on non-Windows / non-x64 hosts (portable syntax checks only)" OFF)
option(CML_BUILD_TESTS "Build the unit tests and register them with ctest" ON)
option(CML_BUILD_GUI "Build the Dear ImGui + D3D11 front-end (broker/cml_gui)" OFF)

# ---------------------------------------------------------------------------
# Platform guard: Chameleon targets Windows x64 only.
# ---------------------------------------------------------------------------
set(_cml_is_win_x64 FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Windows"
   AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|x64|X64)$")
    set(_cml_is_win_x64 TRUE)
endif()
if(NOT _cml_is_win_x64 AND NOT CML_ALLOW_NON_WINDOWS)
    message(FATAL_ERROR
        "Chameleon supports Windows x64 only (detected system '${CMAKE_SYSTEM_NAME}', "
        "processor '${CMAKE_SYSTEM_PROCESSOR}', pointer size ${CMAKE_SIZEOF_VOID_P}). "
        "Use an x64 MSVC or clang-cl toolchain on Windows. To syntax-check the portable "
        "parts on another host, configure with -DCML_ALLOW_NON_WINDOWS=ON.")
endif()
if(NOT _cml_is_win_x64)
    message(WARNING "CML_ALLOW_NON_WINDOWS is set: building portable parts only; the result is not a supported configuration.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# One output directory so exes, the agent DLL and test binaries sit together.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(CmlToolchain)
include(CmlWarnings)
include(CmlTargets)

if(CML_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(third_party)
add_subdirectory(core)
add_subdirectory(agent)
add_subdirectory(broker)

if(CML_BUILD_TESTS)
    add_subdirectory(tests)
endif()
