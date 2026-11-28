# Fixture `cross_module_atomic` (WP-54, R-56): one process, an exe and a plugin DLL that include the same header; the DLL's
# translation unit is refused.
#
#   scale.h      `constexpr int kScale = 1;` and `#define CML_PLUGIN_TLS 0`; holds no code (the broker finds the translation units
#                through its include scan, ADR-0013). Included through shared.h (an inline noinline function returning kScale).
#   fixture_cross_module_atomic.exe         exe_part.cpp    exe_value() = shared_scale() * 10 + kScale      (11)
#                                           main.cpp        the TICK loop; enables the exe and the DLL; never patched
#   fixture_cross_module_atomic_plugin.dll  plugin.cpp      plugin_value() = shared_scale() * 100 + kScale (101); with CML_PLUGIN_TLS 1
#                                                           it also defines and adds a NEW thread_local, which the planner refuses
#                                                           (CML3400)
#                                           plugin_tls.cpp  the DLL's own thread_local (plugin_frames()), never edited
#
# Output: `TICK n=<k> exe=<e> plugin=<p> frames=<the DLL's thread_local count of the main thread>`.
#
# edits/scale_2_tls.edit       kScale 1 -> 2 and CML_PLUGIN_TLS 0 -> 1: exe_part.cpp (in the exe, prepared first) and plugin.cpp (in
#                              the DLL) recompile; plugin.cpp is refused, so neither module is applied (exe=11 plugin=101 stay) and
#                              no patch_applied is published. Before WP-54 the exe was patched (exe=22) while the reply was the refusal.
# edits/stacked/no_tls.edit    on top of it: CML_PLUGIN_TLS back to 0, kScale stays 2: the same edit now reloads both modules
#                              (exe=22 plugin=202). Outside edits/: it applies only on top of the first.
cml_add_fixture_dll(fixture_cross_module_atomic_plugin SOURCES plugin.cpp plugin_tls.cpp)
cml_add_fixture(cross_module_atomic SOURCES main.cpp exe_part.cpp)
target_link_libraries(fixture_cross_module_atomic PRIVATE fixture_cross_module_atomic_plugin)

add_test(NAME fixture.cross_module_atomic.runs COMMAND fixture_cross_module_atomic --ticks 3)
set_tests_properties(fixture.cross_module_atomic.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 exe=11 plugin=101 frames=3"
    TIMEOUT 30)
