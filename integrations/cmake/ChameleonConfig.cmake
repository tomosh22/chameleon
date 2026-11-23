# ChameleonConfig.cmake - installed to <prefix>/lib/cmake/Chameleon/ (WP-29).
#
#   find_package(Chameleon CONFIG REQUIRED)         # CMAKE_PREFIX_PATH or Chameleon_DIR at the install tree
#   add_executable(app main.cpp)
#   chameleon_enable(app)
#
# Provides
#   Chameleon::agent    imported SHARED target: cml_agent.dll, its import library and Chameleon_API.h
#   Chameleon::broker   imported executable: cml_broker.exe
#   Chameleon::ui       imported executable: cml_ui.exe (only when the install has it)
#   chameleon_enable()  integrations/cmake/ChameleonEnable.cmake (installed next to this file)
#   Chameleon_ROOT_DIR  the install prefix, Chameleon_BIN_DIR, Chameleon_INCLUDE_DIR
# Nothing here records an absolute path: the install tree can be moved or unpacked anywhere.

if(DEFINED CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(Chameleon_FOUND FALSE)
    set(Chameleon_NOT_FOUND_MESSAGE "CML1700: Chameleon supports Windows x64 only, but the project is configured for ${CMAKE_SIZEOF_VOID_P}-byte pointers. What to do: configure with an x64 MSVC or clang-cl toolchain (Visual Studio generator: -A x64).")
    return()
endif()

get_filename_component(Chameleon_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(Chameleon_BIN_DIR "${Chameleon_ROOT_DIR}/bin")
set(Chameleon_INCLUDE_DIR "${Chameleon_ROOT_DIR}/include")

if(NOT TARGET Chameleon::agent)
    include("${CMAKE_CURRENT_LIST_DIR}/ChameleonTargets.cmake")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/ChameleonEnable.cmake")
