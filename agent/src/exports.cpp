// The C functions cml_agent.dll exports (resolved by Chameleon_API.h with GetProcAddress).
//
// Rules for everything here: plain C ABI, callable from any thread, never throws (the agent is
// built without exceptions), validates every pointer it is given. The signatures are checked
// against the typedefs in Chameleon_API.h at compile time, so header and DLL cannot drift apart.
#if defined(_WIN32)

#include <new>
#include <type_traits>

#include "Chameleon_API.h"
#include "agent.h"
#include "heap.h"
#include "cml/proc/diag_ids.h"

#define CML_AGENT_EXPORT extern "C" __declspec(dllexport)

namespace {

using cml::agent::Agent;

Agent* as_agent(void* handle) {
    return static_cast<Agent*>(handle);
}

}  // namespace

CML_AGENT_EXPORT int cml_agent_abi_version(void) {
    return CML_AGENT_ABI_VERSION;
}

CML_AGENT_EXPORT cml_status cml_agent_create(const cml_agent_create_params* params, void** out_agent) {
    if (out_agent != nullptr) {
        *out_agent = nullptr;
    }
    if (params == nullptr || out_agent == nullptr || params->struct_size < sizeof(cml_agent_create_params) ||
        params->reserved != 0 || (params->mode != CML_MODE_IMMEDIATE && params->mode != CML_MODE_SYNCHRONIZED)) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    if (params->host_abi_version != CML_AGENT_ABI_VERSION) {
        return CML_STATUS_ABI_MISMATCH;
    }
    Agent* agent = new (std::nothrow) Agent(params->mode);
    if (agent == nullptr) {
        return static_cast<cml_status>(cml::ids::kAgentCreateFailed);
    }
    cml::Result<void> started = agent->start();
    if (!started) {
        const cml_status status = static_cast<cml_status>(started.error().code);
        delete agent;
        return status;
    }
    *out_agent = agent;
    return CML_STATUS_OK;
}

CML_AGENT_EXPORT void cml_agent_destroy(void* agent) {
    if (agent == nullptr) {
        return;
    }
    // Wakes every waiter and waits until all of them have left the agent before freeing it.
    as_agent(agent)->stop_and_wait_for_calls();
    delete as_agent(agent);
}

CML_AGENT_EXPORT cml_status cml_agent_enable_module(void* agent, const wchar_t* path, uint32_t options,
                                                    uint64_t* request_id) {
    if (agent == nullptr || path == nullptr) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    Agent::CallGuard guard(*as_agent(agent));
    return as_agent(agent)->enable_module(path, options, request_id);
}

CML_AGENT_EXPORT cml_status cml_agent_disable_module(void* agent, const wchar_t* path, uint32_t /*options*/,
                                                     uint64_t* request_id) {
    if (agent == nullptr || path == nullptr) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    Agent::CallGuard guard(*as_agent(agent));
    return as_agent(agent)->disable_module(path, request_id);
}

CML_AGENT_EXPORT cml_status cml_agent_wait_request(void* agent, uint64_t request_id, uint32_t timeout_ms,
                                                   cml_reply* out_reply) {
    if (agent == nullptr || out_reply == nullptr) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    Agent::CallGuard guard(*as_agent(agent));
    return as_agent(agent)->wait_request(request_id, timeout_ms, out_reply);
}

CML_AGENT_EXPORT void cml_agent_sync_point(void* agent) {
    if (agent != nullptr) {
        Agent::CallGuard guard(*as_agent(agent));
        as_agent(agent)->sync_point();
    }
}

// Reload, restart and hotkeys arrive with the patch pipeline (later work packages).
CML_AGENT_EXPORT cml_status cml_agent_schedule_reload(void* agent) {
    return agent == nullptr ? CML_STATUS_INVALID_ARGUMENT : CML_STATUS_NOT_IMPLEMENTED;
}

CML_AGENT_EXPORT cml_status cml_agent_wait_for_reload(void* agent, uint32_t /*timeout_ms*/) {
    return agent == nullptr ? CML_STATUS_INVALID_ARGUMENT : CML_STATUS_NOT_IMPLEMENTED;
}

CML_AGENT_EXPORT cml_status cml_agent_schedule_restart(void* agent) {
    return agent == nullptr ? CML_STATUS_INVALID_ARGUMENT : CML_STATUS_NOT_IMPLEMENTED;
}

CML_AGENT_EXPORT cml_status cml_agent_set_global_hotkeys(void* agent, const cml_hotkeys* hotkeys) {
    if (agent == nullptr || hotkeys == nullptr || hotkeys->struct_size < sizeof(cml_hotkeys)) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    return CML_STATUS_NOT_IMPLEMENTED;
}

CML_AGENT_EXPORT int cml_agent_is_connected(void* agent) {
    if (agent == nullptr) {
        return 0;
    }
    Agent::CallGuard guard(*as_agent(agent));
    return as_agent(agent)->is_connected() ? 1 : 0;
}

// --- test-only exports (not part of Chameleon_API.h; the unit tests resolve them by name) -------

// Queues `work(context)` for the application thread: it runs inside the next sync point in
// synchronised mode, immediately on the calling thread in immediate mode.
CML_AGENT_EXPORT cml_status cml_agent_testing_post_work(void* agent, void (*work)(void*), void* context) {
    if (agent == nullptr || work == nullptr) {
        return CML_STATUS_INVALID_ARGUMENT;
    }
    Agent::CallGuard guard(*as_agent(agent));
    return as_agent(agent)->post_main_thread_work(work, context) ? CML_STATUS_OK : CML_STATUS_INVALID_ARGUMENT;
}

// Threads currently blocked in wait_request (lets a test know the waiters are really waiting).
CML_AGENT_EXPORT uint64_t cml_agent_testing_waiters(void* agent) {
    if (agent == nullptr) {
        return 0;
    }
    Agent::CallGuard guard(*as_agent(agent));
    return as_agent(agent)->waiters_for_testing();
}

// Blocks currently allocated from the agent's private heap.
CML_AGENT_EXPORT uint64_t cml_agent_testing_live_heap_blocks(void) {
    return cml::agent::heap::live_blocks();
}

// --- compile-time checks that the exports match the header's typedefs ----------------------------

static_assert(std::is_same_v<decltype(&cml_agent_abi_version), cml_pfn_abi_version>);
static_assert(std::is_same_v<decltype(&cml_agent_create), cml_pfn_create>);
static_assert(std::is_same_v<decltype(&cml_agent_destroy), cml_pfn_destroy>);
static_assert(std::is_same_v<decltype(&cml_agent_enable_module), cml_pfn_request>);
static_assert(std::is_same_v<decltype(&cml_agent_disable_module), cml_pfn_request>);
static_assert(std::is_same_v<decltype(&cml_agent_wait_request), cml_pfn_wait_request>);
static_assert(std::is_same_v<decltype(&cml_agent_sync_point), cml_pfn_sync_point>);
static_assert(std::is_same_v<decltype(&cml_agent_schedule_reload), cml_pfn_call>);
static_assert(std::is_same_v<decltype(&cml_agent_wait_for_reload), cml_pfn_wait_for_reload>);
static_assert(std::is_same_v<decltype(&cml_agent_schedule_restart), cml_pfn_call>);
static_assert(std::is_same_v<decltype(&cml_agent_set_global_hotkeys), cml_pfn_set_hotkeys>);
static_assert(std::is_same_v<decltype(&cml_agent_is_connected), cml_pfn_is_connected>);

#endif  // _WIN32
