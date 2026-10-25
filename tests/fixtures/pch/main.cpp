// Fixture `pch` main loop: prints `TICK n=<k> value=<compute_value()> base=<pch_base()>` every 100 ms.
// `--ticks N` stops after N lines (for ctest); without it the loop runs until killed.
// `--agent` (opt-in, WP-11) also starts the Chameleon agent; see the comments marked AGENT below.
// `--agent-wait MS` bounds how long it waits at exit for the broker's answer (default 30000).
#include "pch.h"  // /Yu: the precompiled header (target_precompile_headers also force-includes it)

#include "logic.h"

// Externals used by logic.cpp (same shape as fixture `basic`).
int g_tick = 0;

void fixture_note(const char* what)
{
    std::printf("NOTE %s\n", what);
}

// AGENT: the agent DLL sits next to this exe (CmlFixture.cmake builds both into one directory).
static std::wstring agent_directory()
{
    const std::wstring self = cml::GetCurrentModulePath();
    const std::size_t slash = self.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : self.substr(0, slash);
}

// AGENT: report the broker's answer to our EnableModule request, once. Returns true when the
// answer has arrived (whether OK or not) so the caller can stop asking.
static bool report_enable(const cml::Agent& agent, const cml::Request& request, DWORD wait_ms)
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

int main(int argc, char** argv)
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
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // AGENT: create it, enable this exe's own module. Neither call waits for the broker.
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
        }
    }

    while (max_ticks < 0 || g_tick < max_ticks) {
        std::printf("TICK n=%d value=%d base=%d\n", g_tick, compute_value(), pch_base());
        ++g_tick;
        agent.SyncPoint();  // AGENT: no-op for an invalid agent and in immediate mode
        if (!enable_reported) {
            enable_reported = report_enable(agent, enable_request, 0);
        }
        Sleep(100);
    }

    if (use_agent) {
        // AGENT: tests need the answer before exit, so wait for it here (bounded). A real
        // application would never block like this.
        if (!enable_reported) {
            enable_reported = report_enable(agent, enable_request, agent_wait_ms);
            if (!enable_reported) {
                std::printf("AGENT enabled=0 status=%d message=\"no answer from the broker\"\n",
                            static_cast<int>(CML_STATUS_TIMEOUT));
            }
        }
        cml::DestroyAgent(agent);
    }
    return 0;
}
