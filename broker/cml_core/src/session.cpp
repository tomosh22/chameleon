#include "cml/core/session.h"

#include <algorithm>
#include <utility>

#include "cml/core/module_event_diag_ids.h"
#include "cml/proc/diag_ids.h"
#include "cml/util/log.h"
#include "cml/util/path.h"

namespace cml::core {

namespace {


ipc::Reply make_reply(std::uint64_t request_id, std::uint32_t status, std::string message) {
    ipc::Reply reply;
    reply.request_id = request_id;
    reply.status = status;
    reply.message = std::move(message);
    return reply;
}

}  // namespace

Session::Session(std::uint64_t agent_id, std::string broker_version)
    : agent_id_(agent_id), broker_version_(std::move(broker_version)) {}

bool Session::same_path(std::string_view a, std::string_view b) {
    return path::same_path(a, b);
}

const ipc::ModuleEntry* Session::find_module(std::string_view path) const {
    // A bare file name ("Game.dll") matches a module by name; anything with a directory must be
    // the same full path.
    for (const ipc::ModuleEntry& module : modules_) {
        if (path::matches_module(path, module.path)) {
            return &module;
        }
    }
    return nullptr;
}

bool Session::is_enabled(std::string_view path) const {
    return std::any_of(enabled_.begin(), enabled_.end(),
                       [path](const std::string& entry) { return same_path(entry, path); });
}

SessionOutcome Session::violation(std::uint64_t request_id, std::string message) {
    CML_LOG(Warn, "agent#%llu protocol violation: %s", static_cast<unsigned long long>(agent_id_),
            message.c_str());
    SessionOutcome outcome;
    outcome.replies.emplace_back(make_reply(request_id, ids::kProtocolViolation, std::move(message)));
    outcome.close = true;
    return outcome;
}

SessionOutcome Session::handle_hello(const ipc::Hello& hello) {
    if (handshaken_) {
        return violation(hello.request_id, "Hello received twice on one connection");
    }
    if (hello.protocol_version != ipc::kProtocolVersion) {
        CML_LOG(Warn, "agent#%llu speaks protocol %u, this broker speaks %u",
                static_cast<unsigned long long>(agent_id_), hello.protocol_version,
                ipc::kProtocolVersion);
        SessionOutcome outcome;
        outcome.replies.emplace_back(make_reply(
            hello.request_id, ids::kHandshakeFailed,
            "protocol version " + std::to_string(hello.protocol_version) +
                " is not supported (broker speaks " + std::to_string(ipc::kProtocolVersion) +
                "); use an agent from the same Chameleon build"));
        outcome.close = true;
        return outcome;
    }
    handshaken_ = true;
    ++revision_;
    pid_ = hello.pid;
    mode_ = hello.mode;
    agent_path_ = hello.agent_path;
    command_line_ = hello.command_line;
    restart_token_ = hello.restart_token;
    agent_abi_version_ = hello.agent_abi_version;

    ipc::Welcome welcome;
    welcome.request_id = hello.request_id;
    welcome.agent_id = agent_id_;
    welcome.broker_version = broker_version_;
    SessionOutcome outcome;
    if (module_events()) {
        // WP-48: only an agent that announced ABI 3 or more gets the trailing field (an older decoder rejects trailing bytes).
        welcome.agent_abi_version = std::min(hello.agent_abi_version, ipc::kAgentAbiVersion);
    } else {
        outcome.warnings.push_back(Diag::make(
            module_events::diag::kAgentWithoutModuleEvents,
            "the agent of process " + std::to_string(pid_) + " speaks agent ABI " + std::to_string(hello.agent_abi_version) +
                ", which reports module changes from a module-list diff every 500 ms: a module freed and loaded again in between is not "
                "seen and keeps running its original code",
            "Use cml_agent.dll from the same Chameleon build as the broker (agent ABI " + std::to_string(ipc::kAgentAbiVersion) +
                "); everything else works with this agent."));
    }
    outcome.replies.emplace_back(std::move(welcome));
    return outcome;
}

ipc::Reply Session::handle_enable(const ipc::EnableModule& request) {
    const ipc::ModuleEntry* module = find_module(request.path);
    if (module == nullptr) {
        const Diag diag = Diag::make(
            ids::kModuleNotLoaded,
            "module '" + request.path + "' is not loaded in process " + std::to_string(pid_),
            "Load the module (or pass its full path) before calling EnableModule.");
        CML_LOG(Warn, "agent#%llu EnableModule request=%llu path=\"%s\" -> %s",
                static_cast<unsigned long long>(agent_id_),
                static_cast<unsigned long long>(request.request_id), request.path.c_str(),
                diag.format().c_str());
        return make_reply(request.request_id, diag.code, diag.message + " " + diag.what_to_do);
    }
    std::string message;
    if (is_enabled(module->path)) {
        message = "module already enabled: " + module->path;
    } else {
        enabled_.push_back(module->path);
        ++revision_;
        message = "module enabled: " + module->path;
    }
    CML_LOG(Info, "agent#%llu EnableModule request=%llu -> OK path=\"%s\"",
            static_cast<unsigned long long>(agent_id_),
            static_cast<unsigned long long>(request.request_id), module->path.c_str());
    return make_reply(request.request_id, 0, std::move(message));
}

ipc::Reply Session::handle_disable(const ipc::DisableModule& request) {
    const ipc::ModuleEntry* module = find_module(request.path);
    const std::string& full_path = module != nullptr ? module->path : request.path;
    const auto it = std::find_if(enabled_.begin(), enabled_.end(), [&full_path](const std::string& e) {
        return same_path(e, full_path);
    });
    if (it == enabled_.end()) {
        const Diag diag = Diag::make(ids::kModuleNotEnabled,
                                     "module '" + request.path + "' is not enabled",
                                     "Call EnableModule for it first.");
        CML_LOG(Warn, "agent#%llu DisableModule request=%llu path=\"%s\" -> %s",
                static_cast<unsigned long long>(agent_id_),
                static_cast<unsigned long long>(request.request_id), request.path.c_str(),
                diag.format().c_str());
        return make_reply(request.request_id, diag.code, diag.message + " " + diag.what_to_do);
    }
    const std::string removed = *it;
    enabled_.erase(it);
    ++revision_;
    CML_LOG(Info, "agent#%llu DisableModule request=%llu -> OK path=\"%s\"",
            static_cast<unsigned long long>(agent_id_),
            static_cast<unsigned long long>(request.request_id), removed.c_str());
    return make_reply(request.request_id, 0, "module disabled: " + removed);
}

void Session::apply_loaded(const ipc::ModuleEntry& module) {
    modules_.erase(std::remove_if(modules_.begin(), modules_.end(), [&module](const ipc::ModuleEntry& e) { return e.base == module.base; }),
                   modules_.end());
    modules_.push_back(module);
    ++revision_;
}

void Session::apply_unloaded(const ipc::ModuleEntry& module) {
    modules_.erase(std::remove_if(modules_.begin(), modules_.end(), [&module](const ipc::ModuleEntry& e) { return e.base == module.base; }),
                   modules_.end());
    ++revision_;
}

void Session::handle_module_events(const ipc::ModuleEvents& events, const std::function<void()>& after_step, SessionOutcome& outcome) {
    for (std::size_t i = 0; i < events.events.size(); ++i) {
        const ipc::ModuleEvent& event = events.events[i];
        if (event.kind == ipc::ModuleEventKind::Loaded) {
            apply_loaded(event.module);
        } else {
            apply_unloaded(event.module);
        }
        ++module_events_applied_;
        // Evidence (greppable, cmltest prints it): the image went and came back at the same base inside one agent report, which a
        // module-list diff cannot see.
        if (event.kind == ipc::ModuleEventKind::Loaded && i > 0 && events.events[i - 1].kind == ipc::ModuleEventKind::Unloaded &&
            events.events[i - 1].module.base == event.module.base) {
            CML_LOG(Info, "WP48-PAIR agent#%llu pid=%u base=0x%llx seq=%llu,%llu module=\"%s\": unloaded and loaded again inside one agent report",
                    static_cast<unsigned long long>(agent_id_), pid_, static_cast<unsigned long long>(event.module.base),
                    static_cast<unsigned long long>(events.events[i - 1].seq), static_cast<unsigned long long>(event.seq),
                    event.module.path.c_str());
        }
        if (after_step) {
            after_step();
        }
    }
    if ((events.flags & ipc::kModuleEventsResync) != 0) {
        ++module_event_resyncs_;
        outcome.warnings.push_back(Diag::make(
            module_events::diag::kModuleEventsResync,
            "the agent of process " + std::to_string(pid_) + " reported " + std::to_string(events.events.size()) +
                " module change(s) from a module-list diff (agent CML" + std::to_string(events.reason) + ", " +
                std::to_string(events.lost) + " loader notification(s) lost): a module freed and loaded again at the same base in that "
                "time was not seen and runs its original code",
            "If a module of this process was freed and loaded again just now, load it again (or hot-restart) to get the session's edits "
            "re-applied; the agent log names the cause (CML" + std::to_string(events.reason) + ")."));
    }
}

SessionOutcome Session::on_message(const ipc::Message& message, const std::function<void()>& after_step) {
    CML_LOG(Info, "agent#%llu <- %s", static_cast<unsigned long long>(agent_id_),
            ipc::describe(message).c_str());

    if (const auto* hello = std::get_if<ipc::Hello>(&message)) {
        return handle_hello(*hello);
    }
    if (!handshaken_) {
        return violation(0, std::string(ipc::to_string(ipc::type_of(message))) +
                                " received before Hello");
    }

    SessionOutcome outcome;
    if (const auto* list = std::get_if<ipc::ModuleList>(&message)) {
        modules_ = list->modules;
        ++revision_;
        CML_LOG(Info, "agent#%llu ModuleList request=%llu recorded %llu modules",
                static_cast<unsigned long long>(agent_id_),
                static_cast<unsigned long long>(list->request_id),
                static_cast<unsigned long long>(modules_.size()));
        outcome.replies.emplace_back(
            make_reply(list->request_id, 0, "recorded " + std::to_string(modules_.size()) + " modules"));
    } else if (const auto* enable = std::get_if<ipc::EnableModule>(&message)) {
        outcome.replies.emplace_back(handle_enable(*enable));
    } else if (const auto* disable = std::get_if<ipc::DisableModule>(&message)) {
        outcome.replies.emplace_back(handle_disable(*disable));
    } else if (const auto* loaded = std::get_if<ipc::ModuleLoaded>(&message)) {
        apply_loaded(loaded->module);
    } else if (const auto* unloaded = std::get_if<ipc::ModuleUnloaded>(&message)) {
        apply_unloaded(unloaded->module);
    } else if (const auto* events = std::get_if<ipc::ModuleEvents>(&message)) {
        handle_module_events(*events, after_step, outcome);  // WP-48
    } else if (const auto* log = std::get_if<ipc::Log>(&message)) {
        CML_LOG(Info, "agent#%llu log[%u]: %s", static_cast<unsigned long long>(agent_id_),
                log->level, log->text.c_str());
    } else if (const auto* exception = std::get_if<ipc::Exception>(&message)) {
        if (exception_sink_) {
            exception_sink_(*exception);
        }
    } else if (std::get_if<ipc::Reply>(&message) != nullptr || std::get_if<ipc::ReserveRangeResult>(&message) != nullptr ||
               std::get_if<ipc::PatchLoaded>(&message) != nullptr || std::get_if<ipc::HooksInstalled>(&message) != nullptr ||
               std::get_if<ipc::RestartInfo>(&message) != nullptr) {
        // Answers to the broker's own requests (WP-12). One whose request id nobody waits for (the request timed
        // out, or the id is made up) is ignored, never a protocol violation.
        if (!answer_sink_ || !answer_sink_(message)) {
            ++ignored_answers_;
            CML_LOG(Debug, "agent#%llu: %s with an unknown request id; ignored", static_cast<unsigned long long>(agent_id_),
                    ipc::to_string(ipc::type_of(message)));
        }
    } else {
        return violation(0, std::string(ipc::to_string(ipc::type_of(message))) +
                                " is not accepted from an agent");
    }
    return outcome;
}

}  // namespace cml::core
