#include "cml/core/log_hub.h"

#include <algorithm>
#include <cstring>

#include "cml/ipc/json.h"
#include "cml/util/path.h"

namespace cml::core {

namespace {
thread_local bool t_in_sink = false;
// WP-58: how long the background writer gathers records into one write (a record reaches the file at most this much later).
constexpr int kGatherMs = 20;

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

LogHub::LogHub(Options options) : options_(options) {
    if (options_.background_writer && options_.file != nullptr) {
        writer_ = std::thread([this] { writer_loop(); });
    }
}

LogHub::~LogHub() {
    if (writer_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        queued_cv_.notify_all();
        writer_.join();  // the writer writes what is queued before it returns
    }
}

void LogHub::writer_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        queued_cv_.wait(lock, [this] { return stop_ || !queued_.empty(); });
        if (queued_.empty()) {
            return;  // stop_, and nothing left to write
        }
        // Gather what the next milliseconds log into one write, unless somebody waits for the file.
        queued_cv_.wait_for(lock, std::chrono::milliseconds(kGatherMs), [this] { return stop_ || urgent_; });
        urgent_ = false;
        std::string batch;
        batch.swap(queued_);
        const std::uint64_t taken = queued_count_;
        lock.unlock();
        std::fwrite(batch.data(), 1, batch.size(), options_.file);
        std::fflush(options_.file);
        lock.lock();
        written_count_ = taken;
        written_cv_.notify_all();
    }
}

void LogHub::wait_written(std::unique_lock<std::mutex>& lock, std::uint64_t count) {
    written_cv_.wait(lock, [this, count] { return written_count_ >= count; });
}

void LogHub::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (writer_.joinable()) {
        urgent_ = true;
        queued_cv_.notify_one();
        wait_written(lock, queued_count_);
    } else if (options_.file != nullptr) {
        std::fflush(options_.file);
    }
}

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
    const int n = std::snprintf(head, sizeof head, "[%7lld ms] [%s] ", static_cast<long long>(r.t_ms), to_string(r.level));
    std::string text;
    text.reserve((n > 0 ? static_cast<std::size_t>(n) : 0) + r.source.size() + 2 + r.message.size() + 1);  // WP-58: one allocation
    text.append(head, n > 0 ? static_cast<std::size_t>(n) : 0);
    text += r.source;
    text += ": ";
    text += r.message;
    return text;
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
    {
        const std::string_view base = path::base_name(std::string_view(file != nullptr ? file : ""));
        char number[16];
        const int n = std::snprintf(number, sizeof number, ":%d", line);
        r.source.reserve(base.size() + (n > 0 ? static_cast<std::size_t>(n) : 0));
        r.source.append(base.data(), base.size());
        r.source.append(number, n > 0 ? static_cast<std::size_t>(n) : 0);
    }
    r.message = message != nullptr ? message : "";
    // Single-line records: embedded newlines (compiler output) stay in JSON as \n; text keeps them verbatim.
    // (WP-58: the line is formatted before the lock is taken, and the record is moved into the ring.)
    const std::string text = options_.file != nullptr ? format(r) : std::string();
    std::string event_text;  // for the bus, when it may want the record
    EventBus* bus = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (bus_ != nullptr && !t_in_sink && static_cast<int>(level) >= static_cast<int>(event_level_)) {
            bus = bus_;
            event_text = r.source + ": " + r.message;
        }
        ring_.push_back(std::move(r));
        while (ring_.size() > options_.ring_size) {
            ring_.pop_front();
        }
        if (options_.file != nullptr && writer_.joinable() && !stop_) {
            // WP-58: queued for the writer; a warning or an error is on disk before the call returns. (Once the hub is being
            // destroyed, a late record is written by its own thread, below.)
            const bool was_empty = queued_.empty();
            queued_ += text;
            queued_ += '\n';
            const std::uint64_t mine = ++queued_count_;
            if (static_cast<int>(level) >= static_cast<int>(LogLevel::Warn)) {
                urgent_ = true;
                queued_cv_.notify_one();
                wait_written(lock, mine);
            } else if (was_empty) {
                queued_cv_.notify_one();  // (the writer waits for a first record, then gathers)
            }
        } else if (options_.file != nullptr) {
            std::fprintf(options_.file, "%s\n", text.c_str());
            if (options_.flush_each) {
                std::fflush(options_.file);
            }
        }
    }
    if (bus != nullptr && bus->wanted("log")) {
        t_in_sink = true;
        bus->publish(make_log_event(lower_level(level).c_str(), event_text));
        t_in_sink = false;
    }
}

}  // namespace cml::core
