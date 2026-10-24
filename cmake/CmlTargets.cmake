# Target "styles" used across the repository.
#
#   cml_target_exceptions_allowed(<t>)  C++ exceptions + RTTI on (tests, GUI, third-party)
#   cml_target_core_style(<t>)          core/ and broker/ non-GUI: no exceptions, no RTTI
#   cml_target_agent_style(<t>)         agent/: static CRT, no exceptions, no RTTI
#   cml_target_fixture_style(<t>)       test fixtures: RelWithDebInfo uses /Ob0 (no inlining)
#   cml_add_core_library(<name> SOURCES ... [LIBS ...])
#
# Exceptions: Chameleon core code reports failure through cml::Result<T>, never by throwing.

function(cml_target_exceptions_allowed target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /EHsc)
    endif()
endfunction()

function(cml_target_core_style target)
    if(MSVC)
        # _HAS_EXCEPTIONS=0 keeps the MSVC STL from warning (C4577) about noexcept without /EH.
        target_compile_options(${target} PRIVATE /EHs-c- /GR-)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
    endif()
endfunction()

function(cml_target_agent_style target)
    cml_target_core_style(${target})
    if(MSVC)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()

function(cml_target_fixture_style target)
    # Fixtures are built with inlining disabled in RelWithDebInfo so that every function stays
    # a distinct patchable symbol. CMake's default RelWithDebInfo flags contain /Ob1; the later
    # /Ob0 wins (cl prints D9025 as a note-level warning that /WX does not escalate; fixtures do
    # not use /WX anyway).
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<CONFIG:RelWithDebInfo>:/Ob0>")
    else()
        target_compile_options(${target} PRIVATE "$<$<CONFIG:RelWithDebInfo>:-fno-inline>")
    endif()
endfunction()

function(cml_add_core_library name)
    cmake_parse_arguments(L "" "" "SOURCES;LIBS" ${ARGN})
    add_library(${name} STATIC ${L_SOURCES})
    target_include_directories(${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    if(L_LIBS)
        target_link_libraries(${name} PUBLIC ${L_LIBS})
    endif()
    cml_apply_warnings(${name})
    cml_target_core_style(${name})
    set_target_properties(${name} PROPERTIES FOLDER "core")
endfunction()
