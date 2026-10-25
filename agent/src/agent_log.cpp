#include "agent_log.h"

#if defined(_WIN32)

#include <windows.h>

#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "cml/proc/proc.h"
#include "cml/util/log.h"

namespace cml::agent {

namespace {

constexpr std::size_t kMaxForwarded = 128;

struct State {
    std::mutex mutex;
    std::FILE* file = nullptr;
    int forwarders = 0;
    std::deque<ipc::Log> queue;
};

// Intentionally leaked: logging can happen during process teardown, after static destructors.
State& state() {
    static State* const instance = new State();
    return *instance;
}

const char* base_name(const char* path) {
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

std::optional<LogLevel> parse_level(const std::string& text) {
    if (text == "trace") return LogLevel::Trace;
    if (text == "debug") return LogLevel::Debug;
    if (text == "info") return LogLevel::Info;
    if (text == "warn") return LogLevel::Warn;
    if (text == "error") return LogLevel::Error;
    return std::nullopt;
}

void sink(void*, LogLevel level, const char* file, int line, const char* message) {
    char buffer[1280];
    const int n = std::snprintf(buffer, sizeof(buffer), "[cml_agent pid=%lu tid=%lu] [%s] %s:%d: %s\n",
                                static_cast<unsigned long>(GetCurrentProcessId()),
                                static_cast<unsigned long>(GetCurrentThreadId()), to_string(level),
                                base_name(file), line, message);
    if (n < 0) {
        return;
    }
    if (IsDebuggerPresent()) {
        OutputDebugStringA(buffer);
    }
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file != nullptr) {
        std::fputs(buffer, s.file);
        std::fflush(s.file);
    }
    if (level >= LogLevel::Warn && s.forwarders > 0) {
        ipc::Log entry;
        entry.level = static_cast<std::uint32_t>(level);
        entry.text = std::string(base_name(file)) + ":" + std::to_string(line) + ": " + message;
        if (entry.text.size() > 2000) {
            entry.text.resize(2000);
            while (!entry.text.empty() && !ipc::is_valid_utf8(entry.text)) {
                entry.text.pop_back();  // do not leave half a UTF-8 sequence behind
            }
        }
        if (s.queue.size() >= kMaxForwarded) {
            s.queue.pop_front();
        }
        s.queue.push_back(std::move(entry));
    }
}

}  // namespace

void install_logging() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    if (const std::optional<std::string> path = proc::get_env("CML_AGENT_LOG"); path && !path->empty()) {
        const std::wstring wide = proc::wide_from_utf8(*path);
        std::FILE* file = nullptr;
        if (_wfopen_s(&file, wide.c_str(), L"ab") == 0) {
            s.file = file;
        }
    }
    LogLevel level = LogLevel::Info;
    if (const std::optional<std::string> text = proc::get_env("CML_LOG_LEVEL"); text) {
        if (const std::optional<LogLevel> parsed = parse_level(*text)) {
            level = *parsed;
        }
    }
    set_log_level(level);
    set_log_sink(&sink, nullptr);
}

void set_log_forwarding(bool enabled) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.forwarders += enabled ? 1 : -1;
    if (s.forwarders <= 0) {
        s.forwarders = 0;
        std::deque<ipc::Log>().swap(s.queue);  // drop the storage too, not just the entries
    }
}

std::vector<ipc::Log> take_forwarded_logs(std::size_t max) {
    std::vector<ipc::Log> out;
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    while (!s.queue.empty() && out.size() < max) {
        out.push_back(std::move(s.queue.front()));
        s.queue.pop_front();
    }
    return out;
}

}  // namespace cml::agent

#endif  // _WIN32
