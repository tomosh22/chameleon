# WP-49 (R-46): the switch objects of tests/unit/broker/test_jump_tables.cpp, compiled by this cell's compiler (cl or
# clang-cl) with flags of their own, whatever the build configuration: /Od, and /O2 /Ob0 (what chameleon_enable() gives a
# RelWithDebInfo build), each as written and with JT_SWAPPED. No /RTC (it cannot be combined with /O2 and would add
# calls), no /GS, no default library; /Gy gives each function its COMDAT section, as in a Chameleon build.
# Included from tests/unit/CMakeLists.txt after cml_test_broker_core is defined (MSVC-style compilers only).
set(_cml_jt_src "${CMAKE_CURRENT_LIST_DIR}/switches.cpp")
set(_cml_jt_dir "${CMAKE_CURRENT_BINARY_DIR}/jump_tables")
set(_cml_jt_objs "")
foreach(_cml_jt_opt Od O2)
    foreach(_cml_jt_variant plain swapped)
        set(_cml_jt_out "${_cml_jt_dir}/switches_${_cml_jt_opt}_${_cml_jt_variant}.obj")
        set(_cml_jt_flags "/${_cml_jt_opt}")
        if(_cml_jt_opt STREQUAL "O2")
            list(APPEND _cml_jt_flags /Ob0)
        endif()
        if(_cml_jt_variant STREQUAL "swapped")
            list(APPEND _cml_jt_flags /DJT_SWAPPED)
        endif()
        add_custom_command(
            OUTPUT "${_cml_jt_out}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_cml_jt_dir}"
            COMMAND "${CMAKE_CXX_COMPILER}" /nologo /c /TP /GS- /Gy /Zl ${_cml_jt_flags} "/Fo${_cml_jt_out}" "${_cml_jt_src}"
            DEPENDS "${_cml_jt_src}"
            COMMENT "WP-49: switches.cpp ${_cml_jt_opt} ${_cml_jt_variant}"
            VERBATIM)
        list(APPEND _cml_jt_objs "${_cml_jt_out}")
    endforeach()
endforeach()
add_custom_target(cml_test_jump_table_objects DEPENDS ${_cml_jt_objs})
set_target_properties(cml_test_jump_table_objects PROPERTIES FOLDER "tests")
add_dependencies(cml_test_broker_core cml_test_jump_table_objects)
target_sources(cml_test_broker_core PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../test_jump_tables.cpp")
target_compile_definitions(cml_test_broker_core PRIVATE "CML_JT_OBJ_DIR=\"${_cml_jt_dir}\"")
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_definitions(cml_test_broker_core PRIVATE CML_JT_COMPILER_IS_CL=1)  # cl.exe, not clang-cl
endif()
