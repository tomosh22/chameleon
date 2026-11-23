// Minimal Chameleon example: a 100 ms loop that calls one function. Edit compute() while the program runs
// (cml_broker reloads the changed file) and the printed value changes without a restart.
#include <Chameleon_API.h>

#include <cstdio>
#include <string>

static int compute(int tick) {
    return tick * 2 + 1;
}

int main() {
    // cml_agent.dll sits next to this executable (chameleon_enable copies it there).
    std::wstring dir = cml::GetCurrentModulePath();
    dir.erase(dir.find_last_of(L"\\/") + 1);

    cml_status status = CML_STATUS_OK;
    cml::Agent agent = cml::CreateSynchronizedAgent(dir.c_str(), &status);
    if (agent) {
        agent.EnableModule(cml::GetCurrentModulePath());
    } else {
        std::printf("no agent (status %d); running without hot reload\n", static_cast<int>(status));
    }

    for (int tick = 0; tick < 50; ++tick) {
        std::printf("TICK n=%d value=%d\n", tick, compute(tick));
        std::fflush(stdout);
        agent.SyncPoint();
        Sleep(100);
    }

    if (agent) {
        cml::DestroyAgent(agent);
    }
    return 0;
}
