// Agent side of InstallHooks / HooksInstalled (WP-10): the part of the request dispatcher that turns
// a broker request into a hook::Installer batch. Kept out of agent.cpp so that file only gains the
// three dispatch lines. Member functions of Agent, declared in agent.h.
#if defined(_WIN32)

#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "agent.h"
#include "cml/proc/diag_ids.h"
#include "cml/util/log.h"
#include "hook/installer.h"
#include "hook/user_hook_ids.h"
#include "hook/user_hooks.h"

namespace cml::agent {

namespace {

// Answers carry the text of a failure only for the first few failed hooks and clip it: a request
// with thousands of failing hooks must still produce a reply that fits in one frame.
constexpr std::size_t kMaxDetailedResults = 64;
constexpr std::size_t kMaxMessageBytes = 600;
constexpr std::size_t kMaxQueuedReplies = 256;

struct HookJob {
    Agent* agent;
    ipc::InstallHooks request;
};

ipc::HookStrategy to_wire(hook::Strategy strategy) {
    switch (strategy) {
        case hook::Strategy::PaddingTwoByte: return ipc::HookStrategy::PaddingTwoByte;
        case hook::Strategy::FiveByte: return ipc::HookStrategy::FiveByte;
        case hook::Strategy::None: break;
    }
    return ipc::HookStrategy::None;
}

ipc::HookResultStatus to_wire(hook::HookStatus status) {
    switch (status) {
        case hook::HookStatus::Installed: return ipc::HookResultStatus::Installed;
        case hook::HookStatus::Repointed: return ipc::HookResultStatus::Repointed;
        case hook::HookStatus::Reverted: return ipc::HookResultStatus::Reverted;
        case hook::HookStatus::Failed: return ipc::HookResultStatus::Failed;
        case hook::HookStatus::NotAttempted: return ipc::HookResultStatus::NotAttempted;
        case hook::HookStatus::Unchanged: return ipc::HookResultStatus::Unchanged;
    }
    return ipc::HookResultStatus::NotAttempted;
}

const void* address_of(std::uint64_t value) {
    return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(value));
}

}  // namespace

void Agent::handle_install_hooks(const ipc::InstallHooks& request) {
    // The request id is repeated by the answer. In synchronised mode the job waits in the SyncPoint
    // queue (the application chooses where its threads are quiescent); in immediate mode
    // post_main_thread_work runs it on this thread at once.
    auto* job = new (std::nothrow) HookJob{this, request};
    if (job == nullptr) {
        ipc::HooksInstalled refusal;
        refusal.request_id = request.request_id;
        refusal.status = ids::kAgentCreateFailed;
        refusal.message = "the agent is out of memory";
        queue_reply(std::move(refusal));
        return;
    }
    if (!post_main_thread_work(&Agent::run_hook_job, job)) {
        delete job;
    }
}

void Agent::run_hook_job(void* context) {
    std::unique_ptr<HookJob> job(static_cast<HookJob*>(context));
    job->agent->apply_install_hooks(job->request);
}

void Agent::apply_install_hooks(const ipc::InstallHooks& request) {
    const bool revert = (request.flags & ipc::kInstallHooksRevert) != 0;
    const std::size_t count = request.hooks.size();
    CML_LOG(Info, "hooks: request %llu: %s %llu function(s)", static_cast<unsigned long long>(request.request_id),
            revert ? "revert" : "install", static_cast<unsigned long long>(count));

    // WP-17: pre-patch hooks of the application run before the first byte is changed, post-patch hooks after
    // the installation succeeded, both on this thread (the SyncPoint thread in synchronised mode). Nothing of
    // this runs while threads are suspended: the installer opens and closes its window inside install().
    const bool run_user_hooks = !revert && count != 0;
    if (run_user_hooks) {
        const std::size_t ran = user_hooks::run_hooks(user_hooks::Kind::PrePatch, enabled_module_paths(), nullptr);
        CML_LOG(Info, "hooks: request %llu: %llu pre-patch hook(s) ran", static_cast<unsigned long long>(request.request_id),
                static_cast<unsigned long long>(ran));
    }

    std::vector<hook::HookRequest> specs(count);
    for (std::size_t i = 0; i < count; ++i) {
        specs[i].target = address_of(request.hooks[i].old_entry);
        specs[i].replacement = revert ? nullptr : address_of(request.hooks[i].new_entry);
        specs[i].name = request.hooks[i].name.c_str();
    }
    std::vector<hook::HookOutcome> outcomes;
    hook::Installer& installer = hook::process_installer();
    const hook::BatchResult batch =
        revert ? installer.revert(specs.data(), count, outcomes) : installer.install(specs.data(), count, outcomes);

    if (run_user_hooks && batch.ok()) {
        const std::size_t ran = user_hooks::run_hooks(user_hooks::Kind::PostPatch, enabled_module_paths(), nullptr);
        CML_LOG(Info, "hooks: request %llu: %llu post-patch hook(s) ran", static_cast<unsigned long long>(request.request_id),
                static_cast<unsigned long long>(ran));
    }

    ipc::HooksInstalled reply;
    reply.request_id = request.request_id;
    reply.status = batch.diag.code;
    if (batch.diag.code != 0) {
        reply.message = ipc::sanitize_utf8(batch.diag.format(), kMaxMessageBytes);
    }
    reply.attempts = batch.stats.attempts;
    reply.retries = batch.stats.retries;
    reply.threads = batch.stats.threads;
    reply.window_ms = batch.stats.window_ms;
    reply.results.reserve(outcomes.size());
    std::size_t detailed = 0;
    for (const hook::HookOutcome& outcome : outcomes) {
        ipc::HookResult result;
        result.strategy = to_wire(outcome.strategy);
        result.status = to_wire(outcome.status);
        result.diag = outcome.diag.code;
        if (outcome.status == hook::HookStatus::Failed && detailed < kMaxDetailedResults) {
            result.message = ipc::sanitize_utf8(outcome.diag.format(), kMaxMessageBytes);
            ++detailed;
        }
        reply.results.push_back(std::move(result));
    }
    queue_reply(std::move(reply));
}

// --- compile notifications (WP-17) ----------------------------------------------------------------------------

namespace {

struct CompileJob {
    Agent* agent;
    ipc::CompileEvent event;
};

}  // namespace

void Agent::handle_compile_event(const ipc::CompileEvent& event) {
    auto* job = new (std::nothrow) CompileJob{this, event};
    if (job == nullptr) {
        CML_LOG(Error, "CML%u: no memory for a compile notification; the hooks of this event are skipped",
                user_hooks::ids::kOutOfMemory);
        return;
    }
    if (!post_main_thread_work(&Agent::run_compile_job, job)) {
        delete job;
    }
}

void Agent::run_compile_job(void* context) {
    std::unique_ptr<CompileJob> job(static_cast<CompileJob*>(context));
    job->agent->apply_compile_event(job->event);
}

void Agent::apply_compile_event(const ipc::CompileEvent& event) {
    user_hooks::Kind kind = user_hooks::Kind::CompileStart;
    const char* text = nullptr;
    switch (event.phase) {
        case ipc::CompilePhase::Start: kind = user_hooks::Kind::CompileStart; break;
        case ipc::CompilePhase::Success: kind = user_hooks::Kind::CompileSuccess; break;
        case ipc::CompilePhase::Error:
            kind = user_hooks::Kind::CompileError;
            text = event.text.c_str();
            break;
    }
    const std::size_t ran = user_hooks::run_hooks(kind, enabled_module_paths(), text);
    CML_LOG(Info, "hooks: %s of %s: %llu hook(s) ran", user_hooks::to_string(kind), event.module.c_str(),
            static_cast<unsigned long long>(ran));
}

std::vector<std::string> Agent::enabled_module_paths() {
    std::vector<std::string> paths;
    std::lock_guard<std::mutex> lock(mutex_);
    paths.reserve(enabled_.size());
    for (const EnabledModule& module : enabled_) {
        paths.push_back(module.path);
    }
    return paths;
}

void Agent::queue_reply(ipc::Message message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (replies_.size() >= kMaxQueuedReplies) {
            replies_.pop_front();  // nobody is collecting them (no connection): keep the newest
        }
        replies_.push_back(std::move(message));
    }
    if (wake_event_ != nullptr) {
        SetEvent(wake_event_);
    }
}

bool Agent::flush_replies(ipc::PipeConnection& connection) {
    for (;;) {
        ipc::Message message;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (replies_.empty()) {
                return true;
            }
            message = std::move(replies_.front());
            replies_.pop_front();
        }
        if (!send_message(connection, message)) {
            // The session is over; answers to its requests mean nothing to the next one.
            std::lock_guard<std::mutex> lock(mutex_);
            replies_.clear();
            return false;
        }
    }
}

}  // namespace cml::agent

#endif  // _WIN32
