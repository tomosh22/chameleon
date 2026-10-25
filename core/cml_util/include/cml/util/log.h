#pragma once

// Minimal logging front-end. The ring buffer + file sink arrive in a later WP; the macro and
// the sink hook below are the stable surface.
//
//   CML_LOG(Info, "attached to pid %u (%s)", pid, name);
//
// The first argument is a LogLevel enumerator name without the scope. Arguments are only
// evaluated when the level is enabled.

namespace cml {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Off };

// A sink receives fully formatted messages. `user` is the pointer given to set_log_sink.
using LogSink = void (*)(void* user, LogLevel level, const char* file, int line, const char* message);

// Thread-safe. The level is atomic; sink and user pointer are published together as one unit.
void set_log_level(LogLevel level) noexcept;
LogLevel log_level() noexcept;
bool log_enabled(LogLevel level) noexcept;

// nullptr restores the default sink (one line to stderr). Safe to call while other threads log.
// Lifetime rule: after set_log_sink returns, a log call that began earlier may still invoke the
// previous sink with the previous `user` pointer, so keep the old `user` object (and sink code)
// alive until no thread can still be inside a log call, e.g. until process exit or a point where
// logging threads are quiescent. LogLevel::Off is a threshold only; CML_LOG(Off, ...) never emits.
void set_log_sink(LogSink sink, void* user) noexcept;

const char* to_string(LogLevel level) noexcept;

#if defined(__GNUC__) || defined(__clang__)
#define CML_PRINTF_LIKE(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))
#else
#define CML_PRINTF_LIKE(fmt_index, first_arg)
#endif

// Formats and dispatches to the sink (messages longer than 1023 bytes are truncated).
void log_write(LogLevel level, const char* file, int line, const char* fmt, ...) noexcept
    CML_PRINTF_LIKE(4, 5);

}  // namespace cml

#define CML_LOG(level, ...)                                                              \
    (::cml::log_enabled(::cml::LogLevel::level)                                          \
         ? ::cml::log_write(::cml::LogLevel::level, __FILE__, __LINE__, __VA_ARGS__)     \
         : static_cast<void>(0))
