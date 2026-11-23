# WP-32: the licensing hook (interface + no-op provider). Called once from the broker's start-up.
cml_add_core_library(cml_license
    SOURCES src/license.cpp
    LIBS cml_util)

# WP-32: docs/TROUBLESHOOTING.md has an entry for every diagnostic id defined in code (fails when one is missing).
if(CML_BUILD_TESTS)
    add_test(NAME docs.troubleshooting
        COMMAND ${CMAKE_COMMAND} -DCML_SOURCE_DIR=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/tools/package/check_troubleshooting.cmake)
endif()
