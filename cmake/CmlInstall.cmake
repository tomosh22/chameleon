# Install and export rules (WP-29): `cmake --install <build> --prefix <dir>` lays out
#
#   bin/                          cml_broker.exe, cml_agent.dll, cml_ui.exe (when built), their PDBs (Debug, RelWithDebInfo)
#   include/Chameleon_API.h
#   lib/cml_agent.lib             import library of the agent
#   lib/cmake/Chameleon/          ChameleonConfig.cmake, ChameleonConfigVersion.cmake, ChameleonTargets*.cmake,
#                                 ChameleonEnable.cmake  (imported targets Chameleon::agent, ::broker, ::ui)
#
# The tree is relocatable: the generated files refer to their own directory only. WP-32 packages exactly this tree.
# Included from the top-level CMakeLists.txt after the broker and the agent are defined.

if(NOT WIN32 OR NOT TARGET cml_agent OR NOT TARGET cml_broker)
    return()
endif()

include(CMakePackageConfigHelpers)

# The agent's build-tree include directory is an absolute source path; an exported target must name it as
# build-interface only and point installed consumers at <prefix>/include.
set_target_properties(cml_agent PROPERTIES
    EXPORT_NAME agent
    INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/agent/include>;$<INSTALL_INTERFACE:include>")
set_target_properties(cml_broker PROPERTIES EXPORT_NAME broker)

# In-tree consumers (the fixtures, tests) can name the same targets as an installed project does.
add_library(Chameleon::agent ALIAS cml_agent)
add_executable(Chameleon::broker ALIAS cml_broker)

install(TARGETS cml_agent EXPORT ChameleonTargets
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
    INCLUDES DESTINATION include)
install(TARGETS cml_broker EXPORT ChameleonTargets RUNTIME DESTINATION bin)
set(_cml_pdb_targets cml_agent cml_broker)
if(TARGET cml_ui)
    set_target_properties(cml_ui PROPERTIES EXPORT_NAME ui)
    install(TARGETS cml_ui EXPORT ChameleonTargets RUNTIME DESTINATION bin)
    list(APPEND _cml_pdb_targets cml_ui)
endif()
foreach(_t IN LISTS _cml_pdb_targets)
    install(FILES "$<TARGET_PDB_FILE:${_t}>" DESTINATION bin CONFIGURATIONS Debug RelWithDebInfo OPTIONAL)
endforeach()
install(FILES "${PROJECT_SOURCE_DIR}/agent/include/Chameleon_API.h" DESTINATION include)

set(_cml_cmake_dir lib/cmake/Chameleon)
install(EXPORT ChameleonTargets NAMESPACE Chameleon:: DESTINATION ${_cml_cmake_dir})
write_basic_package_version_file("${PROJECT_BINARY_DIR}/cmake/ChameleonConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
    ARCH_INDEPENDENT)
install(FILES
    "${PROJECT_SOURCE_DIR}/integrations/cmake/ChameleonConfig.cmake"
    "${PROJECT_SOURCE_DIR}/integrations/cmake/ChameleonEnable.cmake"
    "${PROJECT_BINARY_DIR}/cmake/ChameleonConfigVersion.cmake"
    DESTINATION ${_cml_cmake_dir})
