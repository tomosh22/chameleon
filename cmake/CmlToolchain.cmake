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
    # Same for /GR: RTTI is chosen per target (cml_target_exceptions_allowed adds /GR,
    # core/agent style add /GR-), so no D9025 "overriding /GR with /GR-" noise.
    string(REGEX REPLACE "/GR-?( |$)" " " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(STRIP "${CMAKE_CXX_FLAGS}" CMAKE_CXX_FLAGS)

    # _HAS_EXCEPTIONS=0 for EVERY target (first-party, tests, third-party): inline STL code must be
    # compiled the same way in every translation unit that ends up in one binary, otherwise the
    # same inline function has two definitions (ODR). Tests still enable the language feature
    # with /EHsc so doctest can throw/catch; the STL itself just never throws.
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _HAS_EXCEPTIONS=0)
endif()
