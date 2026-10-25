cml_add_core_library(cml_pdb
    SOURCES
        src/pdb.cpp
        src/types.cpp
        src/path.cpp
        src/index.cpp
        src/index_modules.cpp
        src/index_symbols.cpp
        src/detail/counting.cpp
        src/detail/mapped_file.cpp
        src/detail/msf.cpp
        src/detail/pe_codeview.cpp
    LIBS cml_util cml_third_party_raw_pdb)
# Private headers (src/detail/*.h, src/index_impl.h) are included relative to src/.
target_include_directories(cml_pdb PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(NOT MSVC)
    find_package(Threads REQUIRED)
    target_link_libraries(cml_pdb PUBLIC Threads::Threads)
endif()

# Inspection tool: dumps compilands, build environment, files and symbols (used for evidence in CI).
add_executable(cml_pdb_dump tools/pdb_dump.cpp)
target_link_libraries(cml_pdb_dump PRIVATE cml_pdb)
cml_apply_warnings(cml_pdb_dump)
cml_target_core_style(cml_pdb_dump)
set_target_properties(cml_pdb_dump PROPERTIES FOLDER "tools")
