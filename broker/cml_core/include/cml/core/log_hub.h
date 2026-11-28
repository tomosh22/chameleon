#pragma once

// The broker's structured log sink (WP-12).
//
// Installed with cml::set_log_sink(LogHub::sink, &hub). Every record gets a sequence number and a time stamp
// and goes to
//   * the output file (stdout or --log FILE), as a text line or as JSON lines (--log-format json);
//   * a ring buffer of the most recent records (tail(), for crash reports and the future GUI log view);
//   * the control pipe's `log` events, when an EventBus is attached and somebody subscribed.
// Text line: `[  1234 ms] [INFO] file.cpp:42: message`; JSON line:
// `{"seq":N,"t_ms":N,"level":"info","src":"file.cpp:42","msg":"..."}`. The facility is re-entrancy safe: a log
// call made while a record is being written (an event listener that logs) is written to the file but not
// forwarded again.
//
// WP-58 (R-62): with Options::background_writer the file is written by a thread of the hub. A record is formatted and
// queued in order by the thread that logs it; the writer gathers what is logged within a few milliseconds (kGatherMs) into
// one write and flushes it, so that a stage logging a decision per symbol at trace does not pay a file write per record. A
// record at Warn or above is on disk before its log call returns, as everything logged before it. A caller that reads the
// file itself calls flush() first; the destructor writes what is queued.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cml/core/event_bus.h"
#include "cml/util/log.h"

namespace cml::core {

struct LogRecord {
    std::uint64_t seq = 0;
    double t_ms = 0;
    LogLevel level = LogLevel::Info;
    std::string source;   // "file.cpp:42"
    std::string message;
};

enum class LogFormat { Text, Json };

class LogHub {
public:
    struct Options {
        std::FILE* file = nullptr;  // not owned; nullptr = no file output
        LogFormat format = LogFormat::Text;
        std::size_t ring_size = 2000;
        bool flush_each = true;
        // WP-58: a thread of the hub writes the file (see above). Without it every record is written and flushed by the thread
        // that logs it, which at trace costs that thread a file write per decision.
        bool background_writer = false;
    };

    explicit LogHub(Options options);
    ~LogHub();
    LogHub(const LogHub&) = delete;
    LogHub& operator=(const LogHub&) = delete;

    // Returns when every record logged before the call is in the file (and the file is flushed).
    void flush();

    // The cml::LogSink function; `user` is the LogHub.
    static void sink(void* user, LogLevel level, const char* file, int line, const char* message);

    // Forward records of at least `min_level` as `log` events (nullptr detaches).
    void attach_events(EventBus* bus, LogLevel min_level = LogLevel::Info);

    std::vector<LogRecord> tail(std::size_t count) const;
    std::uint64_t records() const noexcept { return seq_.load(); }

    std::string format(const LogRecord& record) const;
    static std::string format_text(const LogRecord& record);
    static std::string format_json(const LogRecord& record);

private:
    void write(LogLevel level, const char* file, int line, const char* message);
    void writer_loop();
    void wait_written(std::unique_lock<std::mutex>& lock, std::uint64_t count);  // `lock` holds mutex_

    Options options_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> seq_{0};
    mutable std::mutex mutex_;
    std::deque<LogRecord> ring_;
    EventBus* bus_ = nullptr;
    LogLevel event_level_ = LogLevel::Info;
    // WP-58: the background writer's queue (formatted lines in order) and its state, under mutex_.
    std::string queued_;
    std::uint64_t queued_count_ = 0;   // records queued so far
    std::uint64_t written_count_ = 0;  // of those, the ones in the file
    bool stop_ = false;
    bool urgent_ = false;  // a caller waits for the file (flush(), a warning): write without gathering
    std::condition_variable queued_cv_;   // something was queued, urgent_, or stop_
    std::condition_variable written_cv_;  // a batch was written
    std::thread writer_;
};

}  // namespace cml::core
