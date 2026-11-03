// Shared main loop for fixtures added after `basic` (plan section 10).
//
//   cml_fixture::run(argc, argv, tick) prints one TICK line per 100 ms by calling
//   tick(n), where n counts from 0. Options (same as fixture `basic`):
//     --ticks N        stop after N lines (ctest); default: run until killed
//     --agent          load the Chameleon agent next to the exe and enable this module
//     --agent-wait MS  bound the wait for the broker's answer at exit (default 30000)
//     --force-load-collisions N  (WP-16) test option: the agent finds the image range of its first N patch loads
//                      taken, so the broker has to relink at another base (CML_TEST_FORCE_LOAD_COLLISIONS)
//
// Fixtures with DLLs list them in extra_modules() before calling run(): each is enabled next to the exe
// (docs/wp/WP-16.md), and `AGENT module=<file> enabled=1` is printed when the broker answered.
//
// Header-only and only ever included by a fixture's main.cpp, which hot-reload scenarios never
// patch.
#pragma once

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Chameleon_API.h"

namespace cml_fixture {

// File names (beside the exe) of the further modules to enable. Filled by main() before run().
inline std::vector<std::wstring>& extra_modules()
{
    static std::vector<std::wstring> modules;
    return modules;
}

inline std::wstring agent_directory()
{
    const std::wstring self = cml::GetCurrentModulePath();
    const std::size_t slash = self.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : self.substr(0, slash);
}

inline bool report_enable(const cml::Agent& agent, const cml::Request& request, DWORD wait_ms)
{
    cml::Reply reply;
    const cml_status status = agent.WaitForRequest(request, wait_ms, &reply);
    if (status == CML_STATUS_TIMEOUT) {
        return false;
    }
    if (status == CML_STATUS_OK && reply.status == 0) {
        std::printf("AGENT enabled=1 message=\"%s\"\n", reply.message.c_str());
    } else {
        std::printf("AGENT enabled=0 status=%d message=\"%s\"\n",
                    status == CML_STATUS_OK ? static_cast<int>(reply.status) : static_cast<int>(status),
                    reply.message.c_str());
    }
    return true;
}

inline bool report_extra(const cml::Agent& agent, const std::wstring& file, const cml::Request& request, DWORD wait_ms)
{
    cml::Reply reply;
    const cml_status status = agent.WaitForRequest(request, wait_ms, &reply);
    if (status == CML_STATUS_TIMEOUT) {
        return false;
    }
    const std::string name(file.begin(), file.end());  // fixture file names are ASCII
    std::printf("AGENT module=%s enabled=%d status=%d message=\"%s\"\n", name.c_str(),
                status == CML_STATUS_OK && reply.status == 0 ? 1 : 0,
                status == CML_STATUS_OK ? static_cast<int>(reply.status) : static_cast<int>(status), reply.message.c_str());
    return true;
}

template <typename Tick> int run(int argc, char** argv, Tick tick)
{
    long max_ticks = -1;
    bool use_agent = false;
    DWORD agent_wait_ms = 30000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            max_ticks = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--agent") == 0) {
            use_agent = true;
        } else if (std::strcmp(argv[i], "--agent-wait") == 0 && i + 1 < argc) {
            agent_wait_ms = static_cast<DWORD>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--force-load-collisions") == 0 && i + 1 < argc) {
            SetEnvironmentVariableW(L"CML_TEST_FORCE_LOAD_COLLISIONS", std::to_wstring(std::strtol(argv[++i], nullptr, 10)).c_str());
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    struct Extra {
        std::wstring file;
        cml::Request request;
        bool reported;
    };
    std::vector<Extra> extras;
    cml::Agent agent;
    cml::Request enable_request = {CML_STATUS_INVALID_ARGUMENT, 0};
    bool enable_reported = true;
    if (use_agent) {
        cml_status create_status = CML_STATUS_OK;
        agent = cml::CreateDefaultAgent(agent_directory().c_str(), &create_status);
        if (!agent) {
            std::printf("AGENT create failed status=%d\n", static_cast<int>(create_status));
        } else {
            enable_request = agent.EnableModule(cml::GetCurrentModulePath(), cml::ModuleOptions::AllImports);
            enable_reported = false;
            for (const std::wstring& file : extra_modules()) {
                extras.push_back({file, agent.EnableModule(agent_directory() + L"\\" + file, cml::ModuleOptions::AllImports), false});
            }
        }
    }

    for (long n = 0; max_ticks < 0 || n < max_ticks; ++n) {
        tick(static_cast<int>(n));
        agent.SyncPoint();
        if (!enable_reported) {
            enable_reported = report_enable(agent, enable_request, 0);
        }
        for (Extra& extra : extras) {
            if (!extra.reported) {
                extra.reported = report_extra(agent, extra.file, extra.request, 0);
            }
        }
        Sleep(100);
    }

    if (use_agent) {
        if (!enable_reported && !report_enable(agent, enable_request, agent_wait_ms)) {
            std::printf("AGENT enabled=0 status=%d message=\"no answer from the broker\"\n",
                        static_cast<int>(CML_STATUS_TIMEOUT));
        }
        for (Extra& extra : extras) {
            if (!extra.reported) {
                report_extra(agent, extra.file, extra.request, agent_wait_ms);
            }
        }
        cml::DestroyAgent(agent);
    }
    return 0;
}

} // namespace cml_fixture
