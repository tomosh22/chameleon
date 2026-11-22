#include "fault/fault_injection.h"

#include <atomic>
#include <string>

#include "cml/util/diag.h"
#include "cml/util/log.h"

namespace cml::agent::fault {

namespace {

// Constant-initialised, so a thread may read it at any time, whatever the state of the process heap and the loader lock.
std::atomic<std::uint32_t> g_remaining[ipc::fault::kStageCount];

}  // namespace

bool consume(ipc::fault::Stage stage) noexcept {
#if !defined(CML_FAULT_INJECTION)
    (void)stage;
    return false;
#else
    std::atomic<std::uint32_t>& slot = g_remaining[static_cast<std::size_t>(stage)];
    std::uint32_t seen = slot.load(std::memory_order_relaxed);
    while (seen != 0) {
        if (slot.compare_exchange_weak(seen, seen - 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
#endif
}

void arm(ipc::fault::Stage stage, std::uint32_t count) noexcept {
    g_remaining[static_cast<std::size_t>(stage)].store(count, std::memory_order_release);
}

void disarm_all() noexcept {
    for (std::atomic<std::uint32_t>& slot : g_remaining) {
        slot.store(0, std::memory_order_release);
    }
}

std::uint32_t armed(ipc::fault::Stage stage) noexcept {
    return g_remaining[static_cast<std::size_t>(stage)].load(std::memory_order_acquire);
}

bool is_arm_request(const ipc::Message& message) noexcept {
    return std::holds_alternative<ipc::ArmFault>(message);
}

ipc::Message handle_arm_request(const ipc::Message& message) {
    namespace f = ipc::fault;
    const auto* request = std::get_if<ipc::ArmFault>(&message);
    ipc::Reply reply;
    if (request == nullptr) {
        reply.status = f::ids::kAgentBadArm;
        reply.message = "not an ArmFault request";
        return reply;
    }
    reply.request_id = request->request_id;
    if (request->stage == f::kDisarm) {
        disarm_all();  // an agent without injection has nothing armed: success as well
        reply.message = "disarmed";
        return reply;
    }
#if !defined(CML_FAULT_INJECTION)
    reply.status = f::ids::kAgentNotCompiledIn;
    reply.message = "this agent was built without CML_FAULT_INJECTION; use a build with failure injection (the default)";
    return reply;
#else
    const f::StageInfo* info = f::find_stage(request->stage);
    if (info == nullptr || !info->agent_side) {
        reply.status = f::ids::kAgentBadArm;
        reply.message = std::string("'") + request->stage + "' is not a failure-injection stage of the agent (reservation, patch-load, hook-install, post-patch-hooks)";
        return reply;
    }
    if (request->count > f::kMaxCount) {
        reply.status = f::ids::kAgentBadArm;
        reply.message = "count must be 0 (disarm this stage) to " + std::to_string(f::kMaxCount);
        return reply;
    }
    if (request->count == 0) {
        arm(info->stage, 0);
        reply.message = std::string("disarmed ") + info->name;
        return reply;
    }
    arm(info->stage, request->count);
    CML_LOG(Warn, "failure injection: stage '%s' armed for %u pass(es)", info->name, request->count);
    reply.message = std::string("armed ") + info->name;
    return reply;
#endif
}

}  // namespace cml::agent::fault
