#include "cml/util/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace cml {

namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::atomic<LogSink> g_sink{nullptr};
std::atomic<void*> g_user{nullptr};

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
    return static_cast<int>(level) >= g_level.load(std::memory_order_relaxed);
}

void set_log_sink(LogSink sink, void* user) noexcept {
    g_user.store(user, std::memory_order_release);
    g_sink.store(sink, std::memory_order_release);
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

    const LogSink sink = g_sink.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink(g_user.load(std::memory_order_acquire), level, file, line, buffer);
        return;
    }
    std::fprintf(stderr, "[%s] %s:%d: %s\n", to_string(level), base_name(file), line, buffer);
}

}  // namespace cml
