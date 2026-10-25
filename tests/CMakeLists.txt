# The C language is needed for the C99 header check and the c_only fixture (WP-11). It is enabled
# here, on Windows only, so the rest of the build stays C++-only. The project is built with one
# compiler family: when the generator does not pick a C compiler itself (Ninja, Makefiles), use the
# C++ compiler (cl, or clang-cl, which compiles C as well) so that e.g. the clang-cl matrix cell
# does not silently build C with cl.
if(WIN32)
    if(NOT CMAKE_C_COMPILER AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
        set(CMAKE_C_COMPILER "${CMAKE_CXX_COMPILER}")
    endif()
    enable_language(C)
endif()

add_subdirectory(unit)

# Later work packages add these directories (each with its own CMakeLists.txt):
#   tests/fixtures  small console apps, one per feature (see docs/IMPLEMENTATION_PLAN.md section 10)
#   tests/harness   cmltest.exe, which builds fixtures and drives the broker
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/CMakeLists.txt")
    add_subdirectory(fixtures)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/harness/CMakeLists.txt")
    add_subdirectory(harness)
endif()
# Process-level tests (headless broker + fixture with the agent); they need the fixtures above.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/CMakeLists.txt")
    add_subdirectory(integration)
endif()
