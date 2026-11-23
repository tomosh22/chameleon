# coff_cov_extras_objs: see ../CMakeLists.txt. Lives in its own directory so that the optimisation flags of the configuration can be
# dropped for it alone (it is built /Od /Ob0 in every configuration).
if(MSVC)
    _cml_non_debug_configs(_cml_cfgs)
    foreach(_cfg IN LISTS _cml_cfgs)
        string(REGEX REPLACE "(^| )[/-]O[12xbs]?[0-9]?( |$)" " " CMAKE_CXX_FLAGS_${_cfg} "${CMAKE_CXX_FLAGS_${_cfg}}")
        string(REGEX REPLACE "(^| )[/-]O[12xbs]?[0-9]?( |$)" " " CMAKE_CXX_FLAGS_${_cfg} "${CMAKE_CXX_FLAGS_${_cfg}}")
    endforeach()
endif()
add_library(coff_cov_extras_objs OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/../../../fixtures/basic/extras.cpp")
cml_target_exceptions_allowed(coff_cov_extras_objs)
if(MSVC)
    # /Ob0 as well: CMake's RelWithDebInfo flags carry /Ob1, which still inlines at /Od under cl.
    target_compile_options(coff_cov_extras_objs PRIVATE /Od /Ob0 /Gy ${CML_HOTPATCH_FLAGS})
else()
    target_compile_options(coff_cov_extras_objs PRIVATE -O0)
endif()
set_target_properties(coff_cov_extras_objs PROPERTIES FOLDER "tests")
