// See chameleon_corpus.h. Compiled with the project's own flags; no exceptions, no RTTI needed.
#include "chameleon_corpus.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "Chameleon_API.h"

namespace {

struct Marker {
    char name[48];
    long count;
};
constexpr int kMaxMarkers = 24;
Marker g_markers[kMaxMarkers];
volatile long g_marker_lock = 0;  // spin lock: the marker function never allocates

cml::Agent g_agent;
cml::Request g_enable_request = {CML_STATUS_INVALID_ARGUMENT, 0};
bool g_enable_reported = true;
unsigned long long g_frame = 0;
ULONGLONG g_last_print = 0;
FILE* g_log = nullptr;

void out_line(const char* line) {
    std::fputs(line, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (g_log != nullptr) {
        std::fputs(line, g_log);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
}

std::wstring env_w(const wchar_t* name) {
    wchar_t buffer[2048];
    const DWORD n = GetEnvironmentVariableW(name, buffer, 2048);
    if (n == 0 || n >= 2048) {
        return std::wstring();
    }
    return std::wstring(buffer, n);
}

}  // namespace

extern "C" void cml_corpus_marker(const char* name) {
    if (name == nullptr) {
        return;
    }
    while (InterlockedCompareExchange(&g_marker_lock, 1, 0) != 0) {
        YieldProcessor();
    }
    for (int i = 0; i < kMaxMarkers; ++i) {
        if (g_markers[i].name[0] == 0) {
            for (size_t k = 0; k + 1 < sizeof(g_markers[i].name) && name[k] != 0; ++k) {
                g_markers[i].name[k] = name[k];
            }
            g_markers[i].count = 1;
            break;
        }
        if (std::strncmp(g_markers[i].name, name, sizeof(g_markers[i].name) - 1) == 0) {
            ++g_markers[i].count;
            break;
        }
    }
    InterlockedExchange(&g_marker_lock, 0);
}

extern "C" void cml_corpus_init(void) {
    const std::wstring log_path = env_w(L"CML_CORPUS_LOG");
    if (!log_path.empty()) {
        g_log = _wfopen(log_path.c_str(), L"ab");
    }
    if (env_w(L"CML_CORPUS_AGENT") != L"1") {
        return;
    }
    const std::wstring dir = env_w(L"CML_CORPUS_AGENT_DIR");
    cml_status status = CML_STATUS_OK;
    g_agent = cml::CreateDefaultAgent(dir.c_str(), &status);
    char line[256];
    if (!g_agent) {
        std::snprintf(line, sizeof(line), "AGENT create failed status=%d", static_cast<int>(status));
        out_line(line);
        return;
    }
    g_enable_request = g_agent.EnableModule(cml::GetCurrentModulePath(), cml::ModuleOptions::AllImports);
    g_enable_reported = false;
}

extern "C" void cml_corpus_frame(void) {
    ++g_frame;
    g_agent.SyncPoint();
    char line[1024];
    if (!g_enable_reported) {
        cml::Reply reply;
        const cml_status status = g_agent.WaitForRequest(g_enable_request, 0, &reply);
        if (status != CML_STATUS_TIMEOUT) {
            g_enable_reported = true;
            std::snprintf(line, sizeof(line), "AGENT enabled=%d status=%d message=\"%s\"",
                          status == CML_STATUS_OK && reply.status == 0 ? 1 : 0,
                          status == CML_STATUS_OK ? static_cast<int>(reply.status) : static_cast<int>(status),
                          reply.message.c_str());
            out_line(line);
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_print < 200) {
        return;
    }
    g_last_print = now;
    char markers[640];
    markers[0] = 0;
    while (InterlockedCompareExchange(&g_marker_lock, 1, 0) != 0) {
        YieldProcessor();
    }
    for (int i = 0; i < kMaxMarkers && g_markers[i].name[0] != 0; ++i) {
        const size_t used = std::strlen(markers);
        std::snprintf(markers + used, sizeof(markers) - used, "%s%s:%ld", i == 0 ? "" : ",", g_markers[i].name,
                      g_markers[i].count);
    }
    InterlockedExchange(&g_marker_lock, 0);
    std::snprintf(line, sizeof(line), "CORPUS frame=%llu inline=%d tmpl=%d markers=%s", g_frame,
                  cml_corpus_inline_value(), cml_corpus_scale<int>(21) + static_cast<int>(cml_corpus_scale<double>(0.5)),
                  markers[0] != 0 ? markers : "-");
    out_line(line);
}
