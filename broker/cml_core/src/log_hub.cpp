#include "cml/core/log_hub.h"

#include <algorithm>
#include <cstring>

#include "cml/ipc/json.h"
#include "cml/util/path.h"

namespace cml::core {

namespace {
thread_local bool t_in_sink = false;

std::string lower_level(LogLevel level) {
    std::string s = to_string(level);
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}
}  // namespace

LogHub::LogHub(Options options) : options_(options) {}

void LogHub::sink(void* user, LogLevel level, const char* file, int line, const char* message) {
    static_cast<LogHub*>(user)->write(level, file, line, message);
}

void LogHub::attach_events(EventBus* bus, LogLevel min_level) {
    std::lock_guard<std::mutex> lock(mutex_);
    bus_ = bus;
    event_level_ = min_level;
}

std::string LogHub::format_text(const LogRecord& r) {
    char head[64];
    std::snprintf(head, sizeof head, "[%7lld ms] [%s] ", static_cast<long long>(r.t_ms), to_string(r.level));
    return std::string(head) + r.source + ": " + r.message;
}

std::string LogHub::format_json(const LogRecord& r) {
    ipc::json::Value v = ipc::json::Value::object();
    v.set("seq", ipc::json::Value::integer(static_cast<std::int64_t>(r.seq)));
    v.set("t_ms", ipc::json::Value::integer(static_cast<std::int64_t>(r.t_ms)));
    v.set("level", ipc::json::Value::string(lower_level(r.level)));
    v.set("src", ipc::json::Value::string(r.source));
    v.set("msg", ipc::json::Value::string(r.message));
    return ipc::json::serialize(v);
}

std::string LogHub::format(const LogRecord& record) const {
    return options_.format == LogFormat::Json ? format_json(record) : format_text(record);
}

std::vector<LogRecord> LogHub::tail(std::size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t n = std::min(count, ring_.size());
    return std::vector<LogRecord>(ring_.end() - static_cast<std::ptrdiff_t>(n), ring_.end());
}

void LogHub::write(LogLevel level, const char* file, int line, const char* message) {
    LogRecord r;
    r.seq = seq_.fetch_add(1) + 1;
    r.t_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    r.level = level;
    r.source = std::string(path::base_name(std::string_view(file))) + ":" + std::to_string(line);
    r.message = message != nullptr ? message : "";
    // Single-line records: embedded newlines (compiler output) stay in JSON as \n; text keeps them verbatim.
    EventBus* bus = nullptr;
    LogLevel event_level = LogLevel::Info;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ring_.push_back(r);
        while (ring_.size() > options_.ring_size) {
            ring_.pop_front();
        }
        if (options_.file != nullptr) {
            const std::string text = format(r);
            std::fprintf(options_.file, "%s\n", text.c_str());
            if (options_.flush_each) {
                std::fflush(options_.file);
            }
        }
        bus = bus_;
        event_level = event_level_;
    }
    if (bus != nullptr && !t_in_sink && static_cast<int>(level) >= static_cast<int>(event_level) && bus->wanted("log")) {
        t_in_sink = true;
        bus->publish(make_log_event(lower_level(level).c_str(), r.source + ": " + r.message));
        t_in_sink = false;
    }
}

}  // namespace cml::core
