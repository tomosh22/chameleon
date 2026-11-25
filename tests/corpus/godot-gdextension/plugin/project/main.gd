# WP-42 corpus: the host's side of the plugin architecture, run by the Godot 4.5.1 editor binary with
#   --headless --path <this dir> --script res://main.gd -- --ctl=<file>
# Every frame it calls CorpusPlugin.tick() when the plugin is loaded (a call through the host's GDExtensionMethodBind).
# The corpus driver steers the plugin's lifetime through the control file, read every third frame:
#   "loaded <gen>"   load the plugin (LoadLibraryExW inside GDExtensionManager.load_extension); a new <gen> while it is
#                    loaded unloads it (FreeLibrary), waits about 2 s (60 frames, so the agent's 500 ms module poll sees
#                    it gone) and loads it again
#   "unloaded"       unload it
#   anything else    keep the current state (a restarted process starts with the plugin not loaded)
extends SceneTree

const EXT := "res://corpus_plugin.gdextension"

var ctl_path := ""
var frame := 0
var plugin = null
var want_gen := ""
var load_at := -1


func _initialize() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--ctl="):
			ctl_path = a.substr(6).replace("\\", "/")
	print("GDEXT-HOST start ctl=", ctl_path)


func _is_loaded() -> bool:
	return GDExtensionManager.is_extension_loaded(EXT)


func _load() -> void:
	var status = GDExtensionManager.load_extension(EXT)
	plugin = ClassDB.instantiate(&"CorpusPlugin") if _is_loaded() else null
	print("GDEXT-HOST frame=", frame, " load_extension=", status, " instance=", plugin != null)


func _unload() -> void:
	plugin = null  # the last reference: the instance is freed before its class goes away
	var status = GDExtensionManager.unload_extension(EXT)
	print("GDEXT-HOST frame=", frame, " unload_extension=", status)


func _poll_ctl() -> void:
	if ctl_path == "" or not FileAccess.file_exists(ctl_path):
		return
	var text := FileAccess.get_file_as_string(ctl_path).strip_edges()
	if text == "":
		return
	var parts := text.split(" ", false)
	if parts[0] == "loaded":
		var gen: String = parts[1] if parts.size() > 1 else ""
		if gen == want_gen:
			return
		want_gen = gen
		if _is_loaded():
			_unload()
			load_at = frame + 60
		else:
			load_at = frame + 1
	elif parts[0] == "unloaded":
		load_at = -1
		if _is_loaded():
			_unload()


func _process(_delta: float) -> bool:
	frame += 1
	if frame % 3 == 0:
		_poll_ctl()
	if load_at >= 0 and frame >= load_at:
		load_at = -1
		_load()
	if plugin != null:
		plugin.tick(frame)
	return false
