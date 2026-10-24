# Global toolchain defaults. Included once from the top-level CMakeLists.txt.

if(MSVC)
    # CMAKE_MSVC_RUNTIME_LIBRARY is honoured because cmake_minimum_required >= 3.15 (CMP0091 NEW).
    # The whole project uses the static CRT: the agent DLL is injected into arbitrary processes
    # and must not depend on their CRT, and every static library that is linked into the agent
    # (cml_util, cml_proc, ...) must use the same runtime or the link fails with LNK2038.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # CMake seeds CMAKE_CXX_FLAGS with /EHsc. Remove it so that exception handling is an explicit
    # per-target decision (cml_target_exceptions_allowed / cml_target_core_style /
    # cml_target_agent_style) and the compiler never sees conflicting /EH switches (D9025).
    string(REGEX REPLACE "/EH[a-z-]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(STRIP "${CMAKE_CXX_FLAGS}" CMAKE_CXX_FLAGS)

    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
