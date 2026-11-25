// WP-42 corpus plugin: the class the host's script drives every frame. Chameleon's own code, MIT.
#pragma once

#include <godot_cpp/classes/ref_counted.hpp>

class CorpusPlugin : public godot::RefCounted {
    GDCLASS(CorpusPlugin, godot::RefCounted)

protected:
    static void _bind_methods();

public:
    // Called by main.gd once per frame (through the host's GDExtensionMethodBind::call). An edit target.
    int64_t tick(int64_t frame);
};
