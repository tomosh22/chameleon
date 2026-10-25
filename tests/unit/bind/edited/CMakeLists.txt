# WP-07: the EDITED translation unit of every M1 fixture, compiled with the fixture's own flags.
#
# For each fixture the sources are copied to <build>/.../edited/<fixture>/ at configure time, the
# fixture's edits/*.edit (unified diff, -p1) is applied with `git apply --ignore-whitespace` (the same
# tool the harness uses; line endings may differ on Windows checkouts), and the TU named by the diff
# is compiled as an OBJECT library `bind_edit_<fixture>` with the flags cml_add_fixture() gives the
# fixture. The planner tests plan that object against the ORIGINAL fixture module and its PDB.
#
# git must not see the build tree as part of the source repository (the build directory is usually
# inside it): GIT_CEILING_DIRECTORIES stops the repository search above the copy.

# Same as tests/fixtures: fixtures stand in for user projects, which use the normal exception-enabled STL.
get_directory_property(_cml_edit_defs COMPILE_DEFINITIONS)
list(REMOVE_ITEM _cml_edit_defs _HAS_EXCEPTIONS=0)
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_cml_edit_defs}")

find_package(Git REQUIRED)

set_property(GLOBAL PROPERTY CML_BIND_FIXTURE_LINES "")

function(cml_bind_edited_fixture fx)
    set(src "${PROJECT_SOURCE_DIR}/tests/fixtures/${fx}")
    set(copy "${CMAKE_CURRENT_BINARY_DIR}/${fx}")
    file(GLOB edits "${src}/edits/*.edit")
    list(SORT edits)
    list(LENGTH edits n_edits)
    if(n_edits EQUAL 0)
        message(FATAL_ERROR "fixture ${fx} has no edits/*.edit")
    endif()
    list(GET edits 0 edit)

    file(REMOVE_RECURSE "${copy}")
    file(GLOB entries RELATIVE "${src}" "${src}/*")
    foreach(entry IN LISTS entries)
        if(NOT entry STREQUAL "CMakeLists.txt" AND NOT entry STREQUAL "edits")
            file(COPY "${src}/${entry}" DESTINATION "${copy}")
        endif()
    endforeach()

    file(STRINGS "${edit}" plus_lines REGEX "^\\+\\+\\+ b/")
    list(GET plus_lines 0 plus_line)
    string(REGEX REPLACE "^\\+\\+\\+ b/" "" tu "${plus_line}")
    string(STRIP "${tu}" tu)
    string(REGEX REPLACE "[ \t].*$" "" tu "${tu}")

    get_filename_component(copy_parent "${copy}" DIRECTORY)
    file(TO_NATIVE_PATH "${copy_parent}" ceiling)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${ceiling}"
                "${GIT_EXECUTABLE}" apply -p1 --ignore-whitespace "${edit}"
        WORKING_DIRECTORY "${copy}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "applying ${edit} to ${copy} failed (${rc}): ${out} ${err}")
    endif()

    set(objs bind_edit_${fx})
    add_library(${objs} OBJECT "${copy}/${tu}")
    cml_target_exceptions_allowed(${objs})
    cml_target_fixture_style(${objs})
    target_include_directories(${objs} PRIVATE "${PROJECT_SOURCE_DIR}/agent/include" "${PROJECT_SOURCE_DIR}/tests/fixtures/common")
    if(MSVC)
        target_compile_options(${objs} PRIVATE /Gy /hotpatch)
    endif()
    set_target_properties(${objs} PROPERTIES FOLDER "tests/fixtures")

    get_filename_component(edit_name "${edit}" NAME)
    set_property(GLOBAL APPEND PROPERTY CML_BIND_FIXTURE_LINES
        "${fx}|$<TARGET_FILE:fixture_${fx}>|$<TARGET_PDB_FILE:fixture_${fx}>|$<TARGET_OBJECTS:${objs}>|${tu}|${edit_name}")
    set_property(GLOBAL APPEND PROPERTY CML_BIND_FIXTURE_TARGETS fixture_${fx} ${objs})
endfunction()

foreach(fx basic statics virtuals templates lambdas new_function new_global)
    cml_bind_edited_fixture(${fx})
endforeach()
