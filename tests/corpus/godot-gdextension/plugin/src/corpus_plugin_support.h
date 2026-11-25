// WP-42 corpus plugin: the link to the host (see ../../chameleon_plugin_host.inc). Chameleon's own code, MIT.
//
//   corpus_plugin_on_load()    when Godot initialises the extension (scene level): resolves the host's exports, counts
//                              the marker plugin_init and asks the host's agent to enable this DLL (EnableModule).
//   corpus_plugin_poll()       from every tick on the main thread: once the broker answered, the marker
//                              plugin_enabled_ok (or plugin_enable_failed_<status>).
//   corpus_plugin_on_unload()  when Godot deinitialises it: the marker plugin_deinit. The module is NOT disabled: the
//                              broker sees it go away (ModuleUnloaded) and come back (ModuleLoaded) by itself.
//   corpus_plugin_marker(s)    the host's marker list (shown in the CORPUS line the host prints).
#pragma once

void corpus_plugin_on_load();
void corpus_plugin_poll();
void corpus_plugin_on_unload();
void corpus_plugin_marker(const char* name);
