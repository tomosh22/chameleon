// Agent side of the hot-restart (WP-21, plan section 4.12; broker side: broker/cml_core/src/restart.cpp).
//
//   * capture_launch_info(): at start-up the agent records the command line, working directory, environment block and
//     image path of the process, before the application can change them, and CML_RESTART_TOKEN when the broker started
//     this process.
//   * Restart (broker -> agent): the application's CML_PRERESTART_HOOK functions run (WP-17 machinery: same exception
//     containment, same thread rules as the patch hooks), then RestartInfo carries the recorded parameters, the values of
//     the standard handles and the enabled modules to the broker, which terminates the process and starts it again.
//   * ReenableModules (broker -> agent, in the new process): the modules the old process had enabled and the application
//     did not enable by itself are enabled through the normal EnableModule path.
#if defined(_WIN32)

#include <cwchar>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agent.h"
#include "cml/proc/proc.h"
#include "cml/util/log.h"
#include "hook/user_hook_ids.h"
#include "hook/user_hooks.h"
#include "restart_ids.h"

namespace cml::agent {

namespace {

struct RestartJob {
    Agent* agent;
    std::uint64_t request_id;
};

std::u16string to_u16(const wchar_t* text, std::size_t length) {
    return std::u16string(reinterpret_cast<const char16_t*>(text), length);
}

std::uint64_t handle_value(DWORD which) {
    const HANDLE h = GetStdHandle(which);
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(h));
}

}  // namespace

void Agent::capture_launch_info() {
    launch_ = LaunchInfo();
    if (const std::optional<std::string> token = proc::get_env("CML_RESTART_TOKEN"); token) {
        restart_token_ = *token;
    }
    const wchar_t* command_line = GetCommandLineW();
    if (command_line != nullptr) {
        launch_.command_line = to_u16(command_line, std::wcslen(command_line));
    }
    const std::wstring image = proc::read_growing_string([](wchar_t* buffer, DWORD capacity) { return GetModuleFileNameW(nullptr, buffer, capacity); });
    launch_.image_path = to_u16(image.data(), image.size());
    const std::wstring cwd = proc::read_growing_string([](wchar_t* buffer, DWORD capacity) { return GetCurrentDirectoryW(capacity, buffer); });
    launch_.working_directory = to_u16(cwd.data(), cwd.size());
    if (wchar_t* block = GetEnvironmentStringsW(); block != nullptr) {
        const wchar_t* p = block;
        while (*p != L'\0') {
            p += std::wcslen(p) + 1;
        }
        // Everything up to and including the second terminator of "A=b\0C=d\0\0".
        launch_.environment = to_u16(block, static_cast<std::size_t>(p - block) + 1);
        FreeEnvironmentStringsW(block);
    }
    launch_.valid = !launch_.command_line.empty() && !launch_.image_path.empty();
}

void Agent::handle_restart(const ipc::Restart& request) {
    // Dispatched like InstallHooks: the job waits in the SyncPoint queue in synchronised mode (the application chooses
    // where its threads are quiescent), and runs at once on this thread in immediate mode.
    auto* job = new (std::nothrow) RestartJob{this, request.request_id};
    if (job == nullptr) {
        ipc::RestartInfo refusal;
        refusal.request_id = request.request_id;
        refusal.status = restart::ids::kOutOfMemory;
        refusal.message = "the agent is out of memory";
        queue_reply(std::move(refusal));
        return;
    }
    if (!post_main_thread_work(&Agent::run_restart_job, job)) {
        delete job;
    }
}

void Agent::run_restart_job(void* context) {
    std::unique_ptr<RestartJob> job(static_cast<RestartJob*>(context));
    job->agent->apply_restart(job->request_id);
}

void Agent::apply_restart(std::uint64_t request_id) {
    ipc::RestartInfo info;
    info.request_id = request_id;

    const std::size_t units = launch_.image_path.size() + launch_.command_line.size() + launch_.working_directory.size() + launch_.environment.size();
    if (!launch_.valid) {
        info.status = restart::ids::kNoLaunchInfo;
        info.message = "the command line or image path of this process could not be read when the agent started";
        CML_LOG(Error, "CML%u: %s", info.status, info.message.c_str());
    } else if (units > ipc::kMaxLaunchParameterUnits) {
        info.status = restart::ids::kLaunchInfoTooLarge;
        info.message = "the command line and environment (" + std::to_string(units) + " characters) are too large to hand to the broker";
        CML_LOG(Error, "CML%u: %s", info.status, info.message.c_str());
    } else {
        // The application's pre-restart hooks, before anything is terminated. A hook that raises an exception is reported
        // (CML5652) and the next one runs; the restart goes on.
        const HookThread hook_thread_here = synchronized() ? HookThread::None : HookThread::AgentThread;
        {
            std::unique_ptr<user_hooks::Session> session(new (std::nothrow) user_hooks::Session(enabled_module_paths()));
            if (session == nullptr) {
                CML_LOG(Error, "CML%u: no memory for the hook collection; the pre-restart hooks are skipped", user_hooks::ids::kOutOfMemory);
            } else {
                ScopedHookThread scope(hook_thread_here);
                info.hooks_run = static_cast<std::uint32_t>(session->run(user_hooks::Kind::PreRestart, nullptr));
            }
        }
        CML_LOG(Info, "restart: %u pre-restart hook(s) ran; handing the launch parameters to the broker", info.hooks_run);
        info.image_path = launch_.image_path;
        info.command_line = launch_.command_line;
        info.working_directory = launch_.working_directory;
        info.environment = launch_.environment;
        info.std_input = handle_value(STD_INPUT_HANDLE);
        info.std_output = handle_value(STD_OUTPUT_HANDLE);
        info.std_error = handle_value(STD_ERROR_HANDLE);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const EnabledModule& module : enabled_) {
                if (info.modules.size() >= ipc::kMaxRestartModules) {
                    break;
                }
                info.modules.push_back(ipc::RestartModule{module.options, module.path});
            }
        }
    }
    queue_reply(std::move(info));
}

void Agent::handle_reenable(const ipc::ReenableModules& request) {
    std::size_t queued = 0;
    for (const ipc::RestartModule& module : request.modules) {
        const std::wstring wide = proc::wide_from_utf8(module.path);
        if (!wide.empty() && enable_module(wide.c_str(), module.options, nullptr) == CML_STATUS_OK) {
            ++queued;
        } else {
            CML_LOG(Warn, "CML%u: could not queue the enabling of '%s' after the restart", restart::ids::kReenableRefused, module.path.c_str());
        }
    }
    ipc::Reply reply;
    reply.request_id = request.request_id;
    if (queued != request.modules.size()) {
        reply.status = restart::ids::kReenableRefused;
        reply.message = "queued " + std::to_string(queued) + " of " + std::to_string(request.modules.size()) + " modules";
    } else {
        reply.message = "queued " + std::to_string(queued) + " modules";
    }
    CML_LOG(Info, "restart: %s to enable again", reply.message.c_str());
    queue_reply(std::move(reply));
}

}  // namespace cml::agent

#endif  // _WIN32
