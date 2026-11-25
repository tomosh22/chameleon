// WP-42 corpus plugin: the GDExtension entry point (the shape of godot-cpp's test project). Chameleon's own code, MIT.
#include <gdextension_interface.h>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "corpus_plugin.h"
#include "corpus_plugin_support.h"

using namespace godot;

static void initialize_corpus_plugin(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    GDREGISTER_CLASS(CorpusPlugin);
    corpus_plugin_on_load();
}

static void uninitialize_corpus_plugin(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
        return;
    }
    corpus_plugin_on_unload();
}

extern "C" {
GDExtensionBool GDE_EXPORT corpus_plugin_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address,
                                                      GDExtensionClassLibraryPtr p_library,
                                                      GDExtensionInitialization* r_initialization) {
    godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
    init_obj.register_initializer(initialize_corpus_plugin);
    init_obj.register_terminator(uninitialize_corpus_plugin);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
}
}
