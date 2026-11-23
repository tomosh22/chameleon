// Chameleon example: the executable calls into greeter.dll. Both modules are enabled, so a change to either file is
// hot-reloaded into the running process.
#include <Chameleon_API.h>

#include <cstdio>
#include <string>

#include "greeter.h"

int main() {
    const std::wstring self = cml::GetCurrentModulePath();
    std::wstring dir = self;
    dir.erase(dir.find_last_of(L"\\/") + 1);

    cml_status status = CML_STATUS_OK;
    cml::Agent agent = cml::CreateSynchronizedAgent(dir.c_str(), &status);
    if (agent) {
        agent.EnableModule(self);
        agent.EnableModule(dir + L"greeter.dll");
    } else {
        std::printf("no agent (status %d); running without hot reload\n", static_cast<int>(status));
    }

    for (int tick = 0; tick < 50; ++tick) {
        char line[128];
        greeter_message(line, static_cast<int>(sizeof(line)), tick);
        std::printf("%s\n", line);
        std::fflush(stdout);
        agent.SyncPoint();
        Sleep(100);
    }

    if (agent) {
        cml::DestroyAgent(agent);
    }
    return 0;
}
