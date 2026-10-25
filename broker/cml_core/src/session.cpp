#include "cml/core/session.h"

#include <algorithm>
#include <utility>

#include "cml/proc/diag_ids.h"
#include "cml/util/log.h"

namespace cml::core {

namespace {

char fold(char c) noexcept {
    if (c == '/') {
        return '\\';
    }
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

std::string_view strip_extended_prefix(std::string_view path) {
    constexpr std::string_view kPrefix = "\\\\?\\";
    if (path.size() > kPrefix.size() && path.substr(0, kPrefix.size()) == kPrefix) {
        return path.substr(kPrefix.size());
    }
    return path;
}

bool has_separator(std::string_view path) {
    return path.find_first_of("\\/") != std::string_view::npos;
}

std::string_view base_name(std::string_view path) {
    const std::size_t pos = path.find_last_of("\\/");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

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
    a = strip_extended_prefix(a);
    b = strip_extended_prefix(b);
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return fold(x) == fold(y); });
}

const ipc::ModuleEntry* Session::find_module(std::string_view path) const {
    // A bare file name ("Game.dll") matches a module by name; anything with a directory must be
    // the same full path.
    const bool by_name = !has_separator(path);
    for (const ipc::ModuleEntry& module : modules_) {
        if (by_name ? same_path(base_name(module.path), path) : same_path(module.path, path)) {
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
    pid_ = hello.pid;
    agent_path_ = hello.agent_path;
    command_line_ = hello.command_line;

    ipc::Welcome welcome;
    welcome.request_id = hello.request_id;
    welcome.agent_id = agent_id_;
    welcome.broker_version = broker_version_;
    SessionOutcome outcome;
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
    CML_LOG(Info, "agent#%llu DisableModule request=%llu -> OK path=\"%s\"",
            static_cast<unsigned long long>(agent_id_),
            static_cast<unsigned long long>(request.request_id), removed.c_str());
    return make_reply(request.request_id, 0, "module disabled: " + removed);
}

SessionOutcome Session::on_message(const ipc::Message& message) {
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
        modules_.erase(std::remove_if(modules_.begin(), modules_.end(),
                                      [loaded](const ipc::ModuleEntry& e) { return e.base == loaded->module.base; }),
                       modules_.end());
        modules_.push_back(loaded->module);
    } else if (const auto* unloaded = std::get_if<ipc::ModuleUnloaded>(&message)) {
        modules_.erase(std::remove_if(modules_.begin(), modules_.end(),
                                      [unloaded](const ipc::ModuleEntry& e) { return e.base == unloaded->module.base; }),
                       modules_.end());
    } else if (const auto* log = std::get_if<ipc::Log>(&message)) {
        CML_LOG(Info, "agent#%llu log[%u]: %s", static_cast<unsigned long long>(agent_id_),
                log->level, log->text.c_str());
    } else if (std::get_if<ipc::Reply>(&message) != nullptr) {
        // The broker sends no requests yet, so there is nothing a Reply could answer.
        CML_LOG(Warn, "agent#%llu sent an unexpected Reply; ignored",
                static_cast<unsigned long long>(agent_id_));
    } else {
        return violation(0, std::string(ipc::to_string(ipc::type_of(message))) +
                                " is not accepted from an agent");
    }
    return outcome;
}

}  // namespace cml::core
