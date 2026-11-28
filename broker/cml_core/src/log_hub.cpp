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

// The head of a text line, "[   1234 ms] [info] ", into `head`; its length.
std::size_t text_head(const LogRecord& r, char (&head)[64]) {
    const int n = std::snprintf(head, sizeof head, "[%7lld ms] [%s] ", static_cast<long long>(r.t_ms), to_string(r.level));
    return n > 0 ? std::min(static_cast<std::size_t>(n), sizeof head - 1) : 0;
}
}  // namespace

LogHub::LogHub(Options options) : options_(options), background_(options.background_writer && options.file != nullptr) {
    if (background_) {
        writer_ = std::thread([this] { writer_loop(); });
    }
}

LogHub::~LogHub() { stop_writer(); }

void LogHub::stop_writer() {
    if (!background_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            return;  // stopped before
        }
        stop_ = true;
    }
    queued_cv_.notify_all();
    writer_.join();  // the writer writes what is queued, also what is queued while it stops, before it returns
}

void LogHub::writer_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        queued_cv_.wait(lock, [this] { return stop_ || !queued_.empty(); });
        if (queued_.empty()) {
            // stop_, and nothing left to write. From here on (under the lock) a record is written by the thread that logs it,
            // after everything the writer wrote.
            writer_done_ = true;
            return;
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
    if (background_ && !writer_done_) {
        const std::uint64_t pending = queued_count_;
        if (written_count_ < pending) {  // (nothing pending: nothing to hurry)
            urgent_ = true;
            queued_cv_.notify_one();
            wait_written(lock, pending);
            if (queued_.empty()) {
                urgent_ = false;  // what was logged meanwhile is gathered as usual
            }
        }
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
    const std::size_t n = text_head(r, head);
    std::string text;
    text.reserve(n + r.source.size() + 2 + r.message.size() + 1);  // WP-58: one allocation
    text.append(head, n);
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
    // (WP-58: the body of a text line is put together before the lock is taken. The sequence number and the time stamp are
    // taken under the lock, so that they rise in the order of the file and of the ring; the record is moved into the ring.)
    const bool to_file = options_.file != nullptr;
    const bool text_format = options_.format == LogFormat::Text;
    std::string body;
    if (to_file && text_format) {
        body.reserve(r.source.size() + 2 + r.message.size() + 1);
        body += r.source;
        body += ": ";
        body += r.message;
        body += '\n';
    }
    std::string event_text;  // for the bus, when it may want the record
    EventBus* bus = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        r.seq = seq_.fetch_add(1) + 1;
        r.t_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
        char head[64];
        const std::size_t head_size = to_file && text_format ? text_head(r, head) : 0;
        const std::string json = to_file && !text_format ? format_json(r) + "\n" : std::string();
        if (bus_ != nullptr && !t_in_sink && static_cast<int>(level) >= static_cast<int>(event_level_)) {
            bus = bus_;
            event_text = r.source + ": " + r.message;
        }
        ring_.push_back(std::move(r));
        while (ring_.size() > options_.ring_size) {
            ring_.pop_front();
        }
        if (background_ && !writer_done_) {
            // WP-58: queued for the writer, also while the hub is being destroyed (the writer drains the queue before it stops);
            // a warning or an error is on disk before the call returns.
            const bool was_empty = queued_.empty();
            if (text_format) {
                queued_.append(head, head_size);
                queued_ += body;
            } else {
                queued_ += json;
            }
            const std::uint64_t mine = ++queued_count_;
            if (static_cast<int>(level) >= static_cast<int>(LogLevel::Warn)) {
                urgent_ = true;
                queued_cv_.notify_one();
                wait_written(lock, mine);
            } else if (was_empty) {
                queued_cv_.notify_one();  // (the writer waits for a first record, then gathers)
            }
        } else if (to_file) {
            // Written by this thread, under the lock: after everything written before (the writer's last batch included).
            if (text_format) {
                std::fwrite(head, 1, head_size, options_.file);
                std::fwrite(body.data(), 1, body.size(), options_.file);
            } else {
                std::fwrite(json.data(), 1, json.size(), options_.file);
            }
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
