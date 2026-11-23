// Smoke application: prints `SMOKE tick=<n> value=<smoke_value()>` every 100 ms and enables its own module with the
// Chameleon agent. `--agent-dir DIR` names the directory with cml_agent.dll (default: the directory of this exe).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "Chameleon_API.h"
#include "logic.h"

int main(int argc, char** argv)
{
    std::wstring agent_dir;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--agent-dir") == 0) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, nullptr, 0);
            if (n > 1) {
                agent_dir.resize(static_cast<std::size_t>(n));
                MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, agent_dir.data(), n);
                agent_dir.resize(static_cast<std::size_t>(n) - 1);
            }
        }
    }
    if (agent_dir.empty()) {
        const std::wstring self = cml::GetCurrentModulePath();
        const std::size_t slash = self.find_last_of(L"\\/");
        agent_dir = slash == std::wstring::npos ? std::wstring() : self.substr(0, slash);
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    cml_status status = CML_STATUS_OK;
    cml::Agent agent = cml::CreateDefaultAgent(agent_dir.c_str(), &status);
    cml::Request request = {CML_STATUS_INVALID_ARGUMENT, 0};
    bool reported = true;
    if (!agent) {
        std::printf("SMOKE agent=failed status=%d\n", static_cast<int>(status));
    } else {
        request = agent.EnableModule(cml::GetCurrentModulePath(), cml::ModuleOptions::AllImports);
        reported = false;
    }

    for (int tick = 0; tick < 1200; ++tick) {  // 120 s at most: a forgotten smoke run ends by itself
        std::printf("SMOKE tick=%d value=%d\n", tick, smoke_value());
        if (!reported) {
            cml::Reply reply;
            const cml_status wait = agent.WaitForRequest(request, 0, &reply);
            if (wait != CML_STATUS_TIMEOUT) {
                std::printf("SMOKE enabled=%d message=\"%s\"\n", (wait == CML_STATUS_OK && reply.status == 0) ? 1 : 0, reply.message.c_str());
                reported = true;
            }
        }
        Sleep(100);
    }
    cml::DestroyAgent(agent);
    return 0;
}
