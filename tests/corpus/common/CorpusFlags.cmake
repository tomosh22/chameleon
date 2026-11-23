# WP-26: the build flags docs/BUILD_REQUIREMENTS.md (plan 4.2) asks of a project that wants to be hot-reloaded, as
# CMake directory options (they come after CMAKE_<LANG>_FLAGS_<CONFIG>, so /Ob0 beats RelWithDebInfo's /Ob1).
#   compile: /Gy (function-level linking), /hotpatch (first instruction at least 2 bytes), /Ob0 (no stale inlined copies)
#   link:    /DEBUG:FULL, /INCREMENTAL:NO, /OPT:NOREF /OPT:NOICF (every function stays a real body), /FUNCTIONPADMIN
# Not used: /GL, /guard:cf. Include this file BEFORE add_subdirectory() of the project.
if(MSVC)
    add_compile_options(/Gy /hotpatch "$<$<CONFIG:RelWithDebInfo>:/Ob0>")
    add_link_options(/DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN)
endif()
