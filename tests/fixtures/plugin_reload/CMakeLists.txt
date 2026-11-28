# Fixture `plugin_reload` (WP-45, F-42.1..F-42.3): a host that loads a plugin DLL with LoadLibraryW long after its agent sent the
# module list, frees it and loads it again. The reduction of the corpus project godot-gdextension (tests/corpus/repro/f42_plugin_reload).
#
#   fixture_plugin_reload_plugin.dll   plugin.cpp   plugin_value(); the TU the scenarios edit (1 -> 2 -> 3 ...). The host does NOT
#                                                   link it: it is loaded at run time, from the exe's directory
#   fixture_plugin_reload.exe          main.cpp     the TICK loop; `--agent` enables the exe (also in a relaunched process), and the
#                                                   plugin each time it loads it (see main.cpp for the signal file that steers it)
#
# Output: `START pid=<pid> restarted=<0|1>` once; `TICK n=<k> value=<v> loaded=<0|1> gen=<g> pid=<pid> restarted=<0|1>` every 100 ms
# (value 0 while the plugin is not loaded; gen counts the LoadLibraryW calls of this process); `LOADED gen=<g> ok=<0|1> base=<p>`,
# `UNLOADED gen=<g> freed=<0|1> gone=<0|1>[ quick=1]`, `QUICK gap_ms=<ms> same_base=<0|1>` (WP-48, --quick-reload-at) and
# `AGENT exe|plugin enabled=<0|1> ...` when they happen.
#
# Scenarios: plugin_reload (edit, FreeLibrary + LoadLibrary: the edit is still in effect; a second edit links; WP-48: FreeLibrary +
# LoadLibrary inside one agent poll keeps it too; a re-apply that fails is CML5600), plugin_late_load (hot-restart, the new process
# loads the plugin later: the edit is re-applied then; a failure is CML5600).
cml_add_fixture_dll(fixture_plugin_reload_plugin SOURCES plugin.cpp)
cml_add_fixture(plugin_reload SOURCES main.cpp)
add_dependencies(fixture_plugin_reload fixture_plugin_reload_plugin)

add_test(NAME fixture.plugin_reload.runs COMMAND fixture_plugin_reload --ticks 3)
set_tests_properties(fixture.plugin_reload.runs PROPERTIES
    PASS_REGULAR_EXPRESSION "TICK n=2 value=0 loaded=0 gen=0 pid=[0-9]+ restarted=0"
    TIMEOUT 30)
