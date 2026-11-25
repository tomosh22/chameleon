// See corpus_plugin_support.h. Chameleon's own code, MIT.
#include "corpus_plugin_support.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "Chameleon_API.h"

namespace {

using HostAgentFn = void* (*)(void);
using HostMarkerFn = void (*)(const char*);

HostMarkerFn g_marker = nullptr;
cml::Agent g_agent;  // the host's agent: borrowed, never destroyed here
cml::Request g_enable = {CML_STATUS_INVALID_ARGUMENT, 0};
bool g_enable_pending = false;
char g_failed_name[48];
int g_loads_in_this_image = 0;

template <typename T>
T host_export(const char* name) {
    FARPROC p = GetProcAddress(GetModuleHandleW(nullptr), name);
    T out = nullptr;
    static_assert(sizeof(out) == sizeof(p), "function pointer size");
    std::memcpy(&out, &p, sizeof(out));
    return out;
}

}  // namespace

void corpus_plugin_marker(const char* name) {
    if (g_marker != nullptr) {
        g_marker(name);
    }
}

void corpus_plugin_on_load() {
    g_marker = host_export<HostMarkerFn>("cml_corpus_host_marker");
    const HostAgentFn agent_fn = host_export<HostAgentFn>("cml_corpus_host_agent");
    corpus_plugin_marker("plugin_init");
    // A fresh image (FreeLibrary really unmapped the DLL before it was loaded again) starts with this counter at 0.
    ++g_loads_in_this_image;
    corpus_plugin_marker(g_loads_in_this_image == 1 ? "plugin_image_fresh" : "plugin_image_reused");
    if (g_marker == nullptr || agent_fn == nullptr) {
        std::fputs("CORPUS-PLUGIN the host exports cml_corpus_host_agent / cml_corpus_host_marker are missing\n", stdout);
        std::fflush(stdout);
        return;
    }
    g_agent = cml::Agent(static_cast<cml_agent>(agent_fn()));
    if (!g_agent) {
        corpus_plugin_marker("plugin_no_agent");
        return;
    }
    g_enable = g_agent.EnableModule(cml::GetCurrentModulePath(), cml::ModuleOptions::None);
    g_enable_pending = g_enable.status == CML_STATUS_OK;
    if (!g_enable_pending) {
        std::snprintf(g_failed_name, sizeof(g_failed_name), "plugin_enable_not_queued_%d", static_cast<int>(g_enable.status));
        corpus_plugin_marker(g_failed_name);
    }
}

void corpus_plugin_poll() {
    if (!g_enable_pending) {
        return;
    }
    cml::Reply reply;
    const cml_status status = g_agent.WaitForRequest(g_enable, 0, &reply);
    if (status == CML_STATUS_TIMEOUT) {
        return;
    }
    g_enable_pending = false;
    if (status == CML_STATUS_OK && reply.status == 0) {
        corpus_plugin_marker("plugin_enabled_ok");
    } else {
        std::snprintf(g_failed_name, sizeof(g_failed_name), "plugin_enable_failed_%d",
                      static_cast<int>(status == CML_STATUS_OK ? reply.status : status));
        corpus_plugin_marker(g_failed_name);
    }
}

void corpus_plugin_on_unload() {
    corpus_plugin_marker("plugin_deinit");
    g_enable_pending = false;
    g_agent = cml::Agent();
    g_marker = nullptr;
}
