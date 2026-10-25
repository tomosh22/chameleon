# Vendored third-party code. Never compiled with /WX; see third_party/README.md for versions.
# Include directories are SYSTEM so that our strict warnings do not apply to their headers.

# --- xxHash (BSD-2-Clause): header-only use via XXH_INLINE_ALL ---------------------------------
add_library(cml_third_party_xxhash INTERFACE)
target_include_directories(cml_third_party_xxhash SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/xxhash")

# --- doctest (MIT): single header --------------------------------------------------------------
add_library(cml_third_party_doctest INTERFACE)
target_include_directories(cml_third_party_doctest SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/doctest")

# --- raw_pdb (BSD-2-Clause) --------------------------------------------------------------------
file(GLOB _cml_raw_pdb_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/raw_pdb/src/*.cpp")
add_library(cml_third_party_raw_pdb STATIC ${_cml_raw_pdb_sources})
target_include_directories(cml_third_party_raw_pdb SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/raw_pdb/src")
cml_apply_relaxed_warnings(cml_third_party_raw_pdb)
cml_target_exceptions_allowed(cml_third_party_raw_pdb)
set_target_properties(cml_third_party_raw_pdb PROPERTIES FOLDER "third_party")

# --- Dear ImGui (MIT): only when the GUI is requested ------------------------------------------
if(CML_BUILD_GUI)
    set(_cml_imgui_dir "${CMAKE_CURRENT_SOURCE_DIR}/imgui")
    add_library(cml_third_party_imgui STATIC
        "${_cml_imgui_dir}/imgui.cpp"
        "${_cml_imgui_dir}/imgui_draw.cpp"
        "${_cml_imgui_dir}/imgui_tables.cpp"
        "${_cml_imgui_dir}/imgui_widgets.cpp")
    if(WIN32)
        target_sources(cml_third_party_imgui PRIVATE
            "${_cml_imgui_dir}/backends/imgui_impl_win32.cpp"
            "${_cml_imgui_dir}/backends/imgui_impl_dx11.cpp")
    endif()
    target_include_directories(cml_third_party_imgui SYSTEM PUBLIC "${_cml_imgui_dir}" "${_cml_imgui_dir}/backends")
    cml_apply_relaxed_warnings(cml_third_party_imgui)
    cml_target_exceptions_allowed(cml_third_party_imgui)
    set_target_properties(cml_third_party_imgui PROPERTIES FOLDER "third_party")
endif()
