// The agent-thread half of crash -> patch -> continue (WP-20): configuration from the broker, reporting held threads,
// executing the broker's decision. The faulting thread's half is crash/exception_gate.cpp.
#include "agent.h"

#if defined(_WIN32)

#include <string>
#include <vector>

#include "crash/diag_ids.h"
#include "crash/exception_gate.h"
#include "cml/util/log.h"
#include "loader/patch_loader.h"

namespace cml::agent {

static_assert(crash::ids::kUnknownException == ipc::kStatusUnknownException, "the broker's copy of the id differs");

namespace {

const char* action_name(ipc::ExceptionActionKind action) {
    switch (action) {
        case ipc::ExceptionActionKind::Continue: return "continue";
        case ipc::ExceptionActionKind::Leave: return "leave";
        case ipc::ExceptionActionKind::Ignore: return "ignore";
    }
    return "?";
}

}  // namespace

void Agent::install_crash_handler() {
    Result<void> installed = crash::ExceptionGate::shared().install();
    if (!installed) {
        CML_LOG(Warn, "%s", installed.error().format().c_str());
    }
}

void Agent::begin_crash_thread() {
    crash::ExceptionGate& gate = crash::ExceptionGate::shared();
    gate.add_ignored_thread(GetCurrentThreadId());
    gate.set_notify_event(wake_event_);
}

void Agent::end_crash_thread() {
    crash::ExceptionGate& gate = crash::ExceptionGate::shared();
    gate.configure(false, false);  // answers every held thread with Ignore
    gate.set_notify_event(nullptr);
    gate.remove_ignored_thread(GetCurrentThreadId());
}

void Agent::end_crash_session() {
    crash::ExceptionGate::shared().configure(false, false);
}

void Agent::refresh_exception_ranges() {
    ranges_dirty_.store(false, std::memory_order_release);
    const std::vector<std::string> enabled = enabled_module_paths();
    std::vector<crash::Range> ranges;
    ranges.reserve(known_modules_.size() + 8);
    for (const ipc::ModuleEntry& module : known_modules_) {
        for (const std::string& path : enabled) {
            if (same_module(path, module.path)) {
                ranges.push_back(crash::Range{module.base, module.size, false});
                break;
            }
        }
    }
    if (loader::PatchLoader* patch_loader = loader::PatchLoader::shared_if_created()) {
        const loader::PatchLoader::HookLease lease = patch_loader->acquire_hook_lease();
        for (const loader::PatchLoader::HookImage& image : lease.images()) {
            ranges.push_back(crash::Range{image.range.base, image.range.size, true});
        }
    }
    crash::ExceptionGate::shared().set_ranges(ranges.data(), ranges.size());
}

void Agent::refresh_exception_ranges_if_due() {
    if (ranges_dirty_.load(std::memory_order_acquire) && crash::ExceptionGate::shared().enabled()) {
        refresh_exception_ranges();
    }
}

void Agent::handle_exception_config(const ipc::ExceptionConfig& config) {
    crash::ExceptionGate& gate = crash::ExceptionGate::shared();
    ipc::Reply reply;
    reply.request_id = config.request_id;
    const bool enable = (config.flags & ipc::kExceptionConfigEnabled) != 0;
    const bool with_debugger = (config.flags & ipc::kExceptionConfigBreakWithDebugger) != 0;
    if (enable && !gate.installed()) {
        reply.status = crash::ids::kHandlerInstallFailed;
        reply.message = "the crash handler is not installed in this process";
        queue_reply(reply);
        return;
    }
    if (enable) {
        refresh_exception_ranges();  // before the gate opens, so the first fault already finds its module
    }
    gate.configure(enable, with_debugger);
    CML_LOG(Info, "crash handling %s%s", enable ? "on" : "off", enable && with_debugger ? " (also with a debugger attached)" : "");
    reply.message = enable ? "crash handling on" : "crash handling off";
    queue_reply(reply);
}

void Agent::handle_exception_action(const ipc::ExceptionAction& request) {
    crash::Action action = crash::Action::Ignore;
    switch (request.action) {
        case ipc::ExceptionActionKind::Continue: action = crash::Action::Continue; break;
        case ipc::ExceptionActionKind::Leave: action = crash::Action::Leave; break;
        case ipc::ExceptionActionKind::Ignore: action = crash::Action::Ignore; break;
    }
    ipc::Reply reply;
    reply.request_id = request.request_id;
    Result<void> resolved = crash::ExceptionGate::shared().resolve(request.exception_id, action);
    if (!resolved) {
        reply.status = resolved.error().code;
        reply.message = ipc::sanitize_utf8(resolved.error().message + " " + resolved.error().what_to_do, 512);
        CML_LOG(Warn, "exception %u: %s refused: %s", request.exception_id, action_name(request.action), resolved.error().format().c_str());
    } else {
        reply.message = std::string("exception ") + std::to_string(request.exception_id) + ": " + action_name(request.action);
        CML_LOG(Info, "exception %u: %s", request.exception_id, action_name(request.action));
    }
    queue_reply(reply);
}

void Agent::handle_reexecute(const ipc::Reexecute& request) {
    crash::ReexecutePlan plan;
    plan.frame_index = request.frame_index;
    plan.entry_rsp = request.entry_rsp;
    plan.target = request.target;
    plan.param_count = static_cast<std::uint32_t>(request.params.size() < 4 ? request.params.size() : 4);
    for (std::uint32_t i = 0; i < plan.param_count; ++i) {
        const ipc::ReexecuteParam& p = request.params[i];
        plan.params[i].position = p.position;
        plan.params[i].is_float = p.is_float != 0;
        plan.params[i].size = p.size;
        plan.params[i].base_register = p.base_register;
        plan.params[i].offset = p.offset;
    }
    ipc::Reply reply;
    reply.request_id = request.request_id;
    Result<void> resolved = crash::ExceptionGate::shared().resolve_reexecute(request.exception_id, plan);
    if (!resolved) {
        reply.status = resolved.error().code;
        reply.message = ipc::sanitize_utf8(resolved.error().message + " " + resolved.error().what_to_do, 512);
        CML_LOG(Warn, "exception %u: re-execute refused: %s", request.exception_id, resolved.error().format().c_str());
    } else {
        reply.message = std::string("exception ") + std::to_string(request.exception_id) + ": re-execute frame " + std::to_string(request.frame_index);
        CML_LOG(Info, "exception %u: re-execute frame %u at 0x%llx with %u register arguments", request.exception_id, request.frame_index,
                static_cast<unsigned long long>(request.target), plan.param_count);
    }
    queue_reply(reply);
}

bool Agent::flush_exceptions(ipc::PipeConnection& connection) {
    crash::ExceptionGate& gate = crash::ExceptionGate::shared();
    const std::uint32_t overflow = gate.take_overflow_count();
    if (overflow != 0) {
        CML_LOG(Warn, "CML%u: %u faults found no free crash-handler slot and were passed on", crash::ids::kSlotsFull, overflow);
    }
    crash::Held held;
    while (gate.take_new(&held)) {
        ipc::Exception message;
        message.exception_id = held.id;
        message.thread_id = held.thread_id;
        message.code = held.code;
        message.flags = held.in_patch ? ipc::kExceptionFlagInPatch : 0u;
        message.access = held.access;
        message.address = held.address;
        message.data_address = held.data_address;
        message.stack_pointer = held.stack_pointer;
        message.module_base = held.module_base;
        message.frames.reserve(held.frame_count);
        for (std::uint32_t i = 0; i < held.frame_count; ++i) {
            const crash::Frame& f = held.frames[i];
            message.frames.push_back(ipc::ExceptionFrame{f.rip, f.entry_rsp, f.rsp, f.rbp, f.r13, f.function_start});
        }
        for (const ipc::ModuleEntry& module : known_modules_) {
            if (module.base == held.module_base) {
                message.module_path = module.path;
                break;
            }
        }
        CML_LOG(Info, "thread %u faulted with 0x%08X at 0x%llx; holding it as exception %u", held.thread_id, held.code,
                static_cast<unsigned long long>(held.address), held.id);
        if (!send_message(connection, message)) {
            return false;  // the session ends and releases the thread (end_crash_session)
        }
    }
    return true;
}

}  // namespace cml::agent

#endif  // _WIN32
