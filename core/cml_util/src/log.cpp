#include "cml/util/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

namespace cml {

namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
// The sink and its user pointer are published together as one immutable record, so a reader can
// never pair an old sink with a new user pointer. Records are intentionally never freed: a
// concurrent log_write may still be reading the previous one, and set_log_sink is a rare,
// configuration-time call (a few bytes per call).
struct SinkRecord {
    LogSink sink;
    void* user;
};
std::atomic<const SinkRecord*> g_record{nullptr};  // nullptr = default stderr sink

const char* base_name(const char* path) noexcept {
    if (path == nullptr) {
        return "";
    }
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

}  // namespace

void set_log_level(LogLevel level) noexcept {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel log_level() noexcept {
    return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

bool log_enabled(LogLevel level) noexcept {
    // Off is a threshold ("emit nothing"), never a level a message can be logged at.
    if (level == LogLevel::Off) {
        return false;
    }
    return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed);
}

void set_log_sink(LogSink sink, void* user) noexcept {
    if (sink == nullptr) {
        g_record.store(nullptr, std::memory_order_release);
        return;
    }
    const SinkRecord* record = new (std::nothrow) SinkRecord{sink, user};
    if (record == nullptr) {
        return;  // out of memory: keep the previous sink rather than pair sink and user wrongly
    }
    g_record.store(record, std::memory_order_release);
}

const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
        case LogLevel::Off:
            break;
    }
    return "off";
}

void log_write(LogLevel level, const char* file, int line, const char* fmt, ...) noexcept {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (n < 0) {
        static const char kFailed[] = "(log formatting failed)";
        std::memcpy(buffer, kFailed, sizeof(kFailed));
    }

    const SinkRecord* record = g_record.load(std::memory_order_acquire);
    if (record != nullptr) {
        record->sink(record->user, level, file, line, buffer);
        return;
    }
    std::fprintf(stderr, "[%s] %s:%d: %s\n", to_string(level), base_name(file), line, buffer);
}

}  // namespace cml
