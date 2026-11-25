/* F-42.1, F-42.2, F-42.3 (docs/wp/WP-42.md): a host that loads a plugin DLL with LoadLibrary after the agent sent its
 * module list, frees it and loads it again. The reduction of the corpus project godot-gdextension (the Godot editor
 * running a GDExtension). Sources only, like the other repros; build with the flags of docs/BUILD_REQUIREMENTS.md:
 *   cl /Zi /O2 /Ob0 /Gy host.c /link /DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN
 *   cl /Zi /O2 /Ob0 /Gy /LD plugin.c /link /DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN
 * Run with CML_AGENT_DIR=<dir of cml_agent.dll> under `cml_broker --headless`; it prints `TICK n= value=` every 100 ms.
 * Steps (the file host.ctl, read every tick, steers it; write the word in it):
 *   load       LoadLibraryW(plugin.dll); the plugin's init() enables its own module through the host's agent
 *   edit plugin.c `return 1` -> `return 2`, `reload`: value=2 (works)
 *   reload-dll FreeLibrary, 2 s, LoadLibraryW again (the broker logs ModuleUnloaded / ModuleLoaded)
 *     F-42.1: value=1 again, plugin.c still says 2, no diagnostic, and `reload` finds no changed file.
 *   edit plugin.c `return 2` -> `return 3`, `reload`
 *     F-42.2: CML3350 "linking 'patch_1_1' failed ... LNK1104: cannot open file ...\plugin.dll\p<pid>\g1\patch_1_1.dll"
 *     (the patch of the freed instance is still loaded from that path); a second `reload` after another edit links g2.
 *   keep, then `restart`: CML5809 for plugin.dll; then `load`
 *     F-42.3: value=1 although plugin.c holds the last accepted edit; nothing re-applies it, nothing says so.
 * Expected from a correct tool: after `reload-dll` and after the restart's late load the plugin runs the session's
 * edits (re-applied as hot-restart does for modules enabled in time), or a diagnostic names the files that were not
 * re-applied; the first reload after `reload-dll` links. */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "Chameleon_API.h"

static cml_agent g_agent = NULL;

/* The plugin asks for the host's agent through this export (the corpus host exports cml_corpus_host_agent). */
__declspec(dllexport) void* repro_host_agent(void) { return g_agent; }

static void read_ctl(char* word, size_t n) {
    FILE* f = fopen("host.ctl", "rb");
    word[0] = 0;
    if (f != NULL) {
        size_t k = fread(word, 1, n - 1, f);
        word[k] = 0;
        fclose(f);
        word[strcspn(word, " \r\n")] = 0;
    }
}

int main(void) {
    char dir[MAX_PATH] = "";
    GetEnvironmentVariableA("CML_AGENT_DIR", dir, MAX_PATH);
    wchar_t wdir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir, MAX_PATH);
    g_agent = cml_create_agent(wdir, CML_MODE_IMMEDIATE, NULL);
    wchar_t self[MAX_PATH];
    cml_get_current_module_path(self, MAX_PATH, NULL);
    uint64_t id = 0;
    cml_enable_module(g_agent, self, CML_MODULE_NONE, &id);

    HMODULE plugin = NULL;
    int (*value)(void) = NULL;
    char last[32] = "";
    for (int i = 0;; ++i) {
        char word[32];
        read_ctl(word, sizeof(word));
        if (strcmp(word, last) != 0) {
            int loaded_now = 0;
            strcpy(last, word);
            if (strcmp(word, "load") == 0 && plugin == NULL) {
                plugin = LoadLibraryW(L"plugin.dll");
                loaded_now = 1;
            } else if (strcmp(word, "reload-dll") == 0 && plugin != NULL) {
                FreeLibrary(plugin);
                plugin = NULL;
                value = NULL;
                Sleep(2000); /* longer than the agent's 500 ms module poll */
                plugin = LoadLibraryW(L"plugin.dll");
                loaded_now = 1;
            }
            if (loaded_now && plugin != NULL) {
                void (*init)(void) = (void (*)(void))GetProcAddress(plugin, "init");
                value = (int (*)(void))GetProcAddress(plugin, "value");
                if (init != NULL) init();
            }
        }
        printf("TICK n=%d value=%d\n", i, value != NULL ? value() : 0);
        fflush(stdout);
        Sleep(100);
    }
}
