set(_cml_toolchain_sources
    src/toolchain.cpp
    src/command_line.cpp
    src/record.cpp
    src/compile_commands.cpp
    src/rules.cpp
    src/rewrite.cpp
    src/tool_info.cpp
    src/build_flags.cpp
    src/symbol_set.cpp
    src/invoke_common.cpp
    src/link_patch.cpp)
if(WIN32)
    list(APPEND _cml_toolchain_sources src/process_win.cpp src/compile_win.cpp src/link_win.cpp)
endif()

cml_add_core_library(cml_toolchain
    SOURCES ${_cml_toolchain_sources}
    LIBS cml_util cml_pdb cml_coff cml_pe)
if(WIN32)
    target_link_libraries(cml_toolchain PUBLIC version)  # GetFileVersionInfo (tool version fallback)
endif()
if(NOT MSVC)
    find_package(Threads REQUIRED)
    target_link_libraries(cml_toolchain PUBLIC Threads::Threads)
endif()

# cml_tc_probe: evidence tool for CI (recorded commands, rewrites, recompile checks, clang versions).
add_executable(cml_tc_probe tools/tc_probe.cpp)
target_link_libraries(cml_tc_probe PRIVATE cml_toolchain)
cml_apply_warnings(cml_tc_probe)
cml_target_core_style(cml_tc_probe)
set_target_properties(cml_tc_probe PROPERTIES FOLDER "tools")
