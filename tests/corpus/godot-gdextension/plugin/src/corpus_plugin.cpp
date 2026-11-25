// WP-42 corpus plugin. Chameleon's own code, MIT. The corpus edits insert marker lines after the plugin_tick line.
#include "corpus_plugin.h"

#include <godot_cpp/core/class_db.hpp>

#include "corpus_plugin_support.h"

void CorpusPlugin::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("tick", "frame"), &CorpusPlugin::tick);
}

int64_t CorpusPlugin::tick(int64_t frame) {
    corpus_plugin_poll();
    corpus_plugin_marker("plugin_tick");
    return frame + 1;
}
