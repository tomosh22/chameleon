// The event bus and the structured log sink (WP-12).
#include <atomic>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../pdb/test_util.h"
#include "cml/core/event_bus.h"
#include "cml/core/log_hub.h"
#include "cml/ipc/json.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;

namespace {

class Collect final : public EventListener {
public:
    void on_event(const Event& e) override { events.push_back(e); }
    std::vector<Event> events;
};

std::FILE* open_scratch(const std::string& path) {
#if defined(_WIN32)
    std::FILE* f = nullptr;
    return fopen_s(&f, path.c_str(), "w+b") == 0 ? f : nullptr;
#else
    return std::fopen(path.c_str(), "w+b");
#endif
}

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The lines of `text` that hold `tag` followed by a number, those numbers in file order.
std::vector<int> numbers_after(const std::string& text, const std::string& tag) {
    std::vector<int> out;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const std::size_t k = line.find(tag);
        if (k != std::string::npos) {
            out.push_back(std::stoi(line.substr(k + tag.size())));
        }
    }
    return out;
}

// Restores the process-wide log sink and level.
struct SinkGuard {
    LogLevel level = log_level();
    ~SinkGuard() {
        set_log_sink(nullptr, nullptr);
        set_log_level(level);
    }
};

}  // namespace

TEST_CASE("diag events carry the stable id, message, action, severity and stage") {
    const Diag d = Diag::make(3200, "unresolved external symbol 'foo' referenced from 'bar'", "Define it.");
    const Event e = make_diag_event(d, "error", "plan", 4242);
    CHECK(e.name == "diag");
    CHECK(e.body.get_string("event") == "diag");
    CHECK(e.body.get_string("id") == "CML3200");
    CHECK(e.body.get_string("message") == d.message);
    CHECK(e.body.get_string("action") == "Define it.");
    CHECK(e.body.get_string("severity") == "error");
    CHECK(e.body.get_string("stage") == "plan");
    CHECK(e.body.get_integer("pid") == 4242);
    // The line is one JSON line without raw newlines.
    CHECK(ipc::json::serialize(e.body).find('\n') == std::string::npos);
    // No pid, no stage: the members are left out.
    const Event bare = make_diag_event(d, "warning", "", 0);
    CHECK(bare.body.find("pid") == nullptr);
    CHECK(bare.body.find("stage") == nullptr);
}

TEST_CASE("patch_applied and log events") {
    const Event p = make_patch_applied_event(7, "game.exe", 3, 4, 1, 2);
    CHECK(p.name == "patch_applied");
    CHECK(p.body.get_integer("pid") == 7);
    CHECK(p.body.get_string("module") == "game.exe");
    CHECK(p.body.get_integer("generation") == 3);
    CHECK(p.body.get_integer("hooks") == 4);
    const Event l = make_log_event("warn", "something");
    CHECK(l.body.get_string("level") == "warn");
    CHECK(l.body.get_string("message") == "something");
}

TEST_CASE("subscribers get the events they asked for") {
    EventBus bus;
    auto all = std::make_shared<Collect>();
    auto only_diag = std::make_shared<Collect>();
    const EventBus::Id id_all = bus.subscribe(all);
    const EventBus::Id id_diag = bus.subscribe(only_diag, {"diag"});
    CHECK(bus.subscribers() == 2);
    CHECK(bus.wanted("log"));
    bus.publish(make_log_event("info", "x"));
    bus.publish(make_diag_event(Diag::make(3200, "m", "a"), "error", "plan"));
    CHECK(all->events.size() == 2);
    REQUIRE(only_diag->events.size() == 1);
    CHECK(only_diag->events[0].name == "diag");

    CHECK(bus.update(id_diag, {"patch_applied"}));
    bus.publish(make_diag_event(Diag::make(3200, "m", "a"), "error", "plan"));
    CHECK(only_diag->events.size() == 1);
    CHECK(all->events.size() == 3);

    bus.unsubscribe(id_all);
    bus.unsubscribe(id_diag);
    CHECK(bus.subscribers() == 0);
    CHECK_FALSE(bus.wanted("diag"));
    bus.publish(make_log_event("info", "nobody listens"));
    CHECK(all->events.size() == 3);
    CHECK_FALSE(bus.update(999, {}));
}

TEST_CASE("a listener may unsubscribe itself while it is being called") {
    EventBus bus;
    struct SelfRemoving final : EventListener {
        EventBus* bus = nullptr;
        EventBus::Id id = 0;
        int calls = 0;
        void on_event(const Event&) override {
            ++calls;
            bus->unsubscribe(id);
        }
    };
    auto l = std::make_shared<SelfRemoving>();
    l->bus = &bus;
    l->id = bus.subscribe(l);
    bus.publish(make_log_event("info", "1"));
    bus.publish(make_log_event("info", "2"));
    CHECK(l->calls == 1);
}

TEST_CASE("log hub: text and JSON lines, sequence numbers, ring buffer") {
    SinkGuard guard;
    testpdb::TempDir dir;
    std::FILE* file = open_scratch(dir.file("log.txt"));
    REQUIRE(file != nullptr);
    LogHub::Options options;
    options.file = file;
    options.ring_size = 3;
    LogHub text_hub(options);
    set_log_level(LogLevel::Trace);
    set_log_sink(LogHub::sink, &text_hub);
    CML_LOG(Info, "first %d", 1);
    CML_LOG(Warn, "second");
    CML_LOG(Error, "third");
    CML_LOG(Debug, "fourth");
    set_log_sink(nullptr, nullptr);
    CHECK(text_hub.records() == 4);
    const std::vector<LogRecord> tail = text_hub.tail(10);
    REQUIRE(tail.size() == 3);  // ring of 3: the oldest record fell out
    CHECK(tail[0].message == "second");
    CHECK(tail[2].message == "fourth");
    CHECK(tail[2].seq == 4);
    CHECK(tail[0].source.find("test_events_and_log.cpp:") == 0);
    const std::string line = LogHub::format_text(tail[1]);
    CHECK(line.find("[error]") != std::string::npos);
    CHECK(line.find("third") != std::string::npos);
    CHECK(text_hub.tail(1).size() == 1);

    const std::string json = LogHub::format_json(tail[1]);
    Result<ipc::json::Value> parsed = ipc::json::parse(json);
    REQUIRE(parsed.has_value());
    CHECK(parsed->get_string("level") == "error");
    CHECK(parsed->get_string("msg") == "third");
    CHECK(parsed->get_integer("seq") == 3);
    CHECK(json.find('\n') == std::string::npos);

    // What went to the file.
    std::rewind(file);
    char buf[512] = {};
    const std::size_t n = std::fread(buf, 1, sizeof buf - 1, file);
    const std::string written(buf, n);
    CHECK(written.find("first 1") != std::string::npos);
    CHECK(written.find("[warn]") != std::string::npos);
    std::fclose(file);
}

TEST_CASE("log hub: JSON mode writes one JSON object per record, and a message with a newline stays on one line") {
    SinkGuard guard;
    testpdb::TempDir dir;
    std::FILE* file = open_scratch(dir.file("log.json"));
    REQUIRE(file != nullptr);
    LogHub::Options options;
    options.file = file;
    options.format = LogFormat::Json;
    LogHub hub(options);
    set_log_level(LogLevel::Info);
    set_log_sink(LogHub::sink, &hub);
    CML_LOG(Info, "line one\nline two");
    set_log_sink(nullptr, nullptr);
    std::rewind(file);
    char buf[512] = {};
    const std::size_t n = std::fread(buf, 1, sizeof buf - 1, file);
    std::string out(buf, n);
    REQUIRE_FALSE(out.empty());
    CHECK(out.back() == '\n');
    out.pop_back();
    CHECK(out.find('\n') == std::string::npos);
    Result<ipc::json::Value> parsed = ipc::json::parse(out);
    REQUIRE(parsed.has_value());
    CHECK(parsed->get_string("msg") == "line one\nline two");
    std::fclose(file);
}

TEST_CASE("log hub forwards records to subscribers as log events and does not recurse") {
    SinkGuard guard;
    EventBus bus;
    auto sub = std::make_shared<Collect>();
    bus.subscribe(sub, {"log"});
    LogHub::Options options;
    LogHub hub(options);
    hub.attach_events(&bus, LogLevel::Warn);
    set_log_level(LogLevel::Trace);
    set_log_sink(LogHub::sink, &hub);
    CML_LOG(Info, "below the event level");
    CML_LOG(Warn, "forwarded");
    // A listener that logs while it is called must not be forwarded again (no recursion).
    struct Chatty final : EventListener {
        void on_event(const Event&) override { CML_LOG(Warn, "from a listener"); }
    };
    auto chatty = std::make_shared<Chatty>();
    bus.subscribe(chatty, {"log"});
    CML_LOG(Error, "second");
    set_log_sink(nullptr, nullptr);
    REQUIRE(sub->events.size() == 2);
    CHECK(sub->events[0].body.get_string("level") == "warn");
    CHECK(sub->events[0].body.get_string("message").find("forwarded") != std::string::npos);
    CHECK(sub->events[1].body.get_string("level") == "error");
}

// WP-58 (R-62): the background writer keeps the records in order, writes a warning before its log call returns, and leaves
// nothing behind when the hub is flushed or destroyed.
TEST_CASE("log hub: the background writer writes every record in order, a warning at once") {
    SinkGuard guard;
    testpdb::TempDir dir;
    const std::string path = dir.file("log_bg.txt");
    std::FILE* file = open_scratch(path);
    REQUIRE(file != nullptr);
    {
        LogHub::Options options;
        options.file = file;
        options.background_writer = true;
        LogHub hub(options);
        set_log_level(LogLevel::Trace);
        set_log_sink(LogHub::sink, &hub);
        std::thread other([] {
            for (int i = 0; i < 500; ++i) {
                CML_LOG(Trace, "other %d", i);
            }
        });
        for (int i = 0; i < 500; ++i) {
            CML_LOG(Trace, "main %d", i);
        }
        other.join();
        CML_LOG(Warn, "a warning");
        {
            // On disk before the call returned, with everything logged before it.
            const std::string now = read_text(path);
            CHECK(now.find("a warning") != std::string::npos);
            CHECK(now.find("main 499") != std::string::npos);
            CHECK(now.find("other 499") != std::string::npos);
        }
        CML_LOG(Debug, "after the warning");
        hub.flush();
        CHECK(read_text(path).find("after the warning") != std::string::npos);
        CML_LOG(Trace, "last");
        set_log_sink(nullptr, nullptr);
        CHECK(hub.records() == 1003);
    }  // the destructor writes what is still queued
    const std::string all = read_text(path);
    CHECK(all.find("last") != std::string::npos);
    // One line per record, each thread's records in the order it logged them.
    std::size_t lines = 0;
    std::vector<std::pair<std::string, int>> next = {{": main ", 0}, {": other ", 0}};
    bool ordered = true;
    for (std::size_t at = 0; at < all.size();) {
        const std::size_t end = all.find('\n', at);
        REQUIRE(end != std::string::npos);
        const std::string line = all.substr(at, end - at);
        at = end + 1;
        ++lines;
        for (auto& [tag, expected] : next) {
            const std::size_t k = line.find(tag);
            if (k != std::string::npos) {
                ordered = ordered && std::stoi(line.substr(k + tag.size())) == expected;
                ++expected;
            }
        }
    }
    CHECK(lines == 1003);
    CHECK(ordered);
    CHECK(next[0].second == 500);
    CHECK(next[1].second == 500);
    std::fclose(file);
}

// WP-58 (review, findings 1 and 2): another thread logs while the background writer stops (what the destructor does first).
// Every record reaches the file once, in the order that thread logged them: those queued before the writer stopped are in
// its batches, the later ones are written by the logging thread after the writer's last batch. Under TSan this is also the
// race of the writer's join against the logging thread (the mode is decided once, in the constructor).
TEST_CASE("log hub: a record logged while the background writer stops is written after the earlier ones, in order") {
    SinkGuard guard;
    testpdb::TempDir dir;
    const std::string path = dir.file("log_stop.txt");
    std::FILE* file = open_scratch(path);
    REQUIRE(file != nullptr);
    constexpr int kRecords = 4000;
    {
        LogHub::Options options;
        options.file = file;
        options.background_writer = true;
        LogHub hub(options);
        std::atomic<int> logged{0};
        std::thread other([&] {
            for (int i = 0; i < kRecords; ++i) {
                LogHub::sink(&hub, LogLevel::Trace, __FILE__, __LINE__, ("late " + std::to_string(i)).c_str());
                logged.store(i + 1);
                if (i % 997 == 0) {
                    LogHub::sink(&hub, LogLevel::Warn, __FILE__, __LINE__, ("late " + std::to_string(++i)).c_str());
                    logged.store(i + 1);
                }
            }
        });
        while (logged.load() < kRecords / 4) {
            std::this_thread::yield();
        }
        hub.stop_writer();  // while `other` logs
        const int when_stopped = logged.load();
        other.join();
        CHECK(when_stopped < kRecords);  // (the writer stopped while the other thread was logging)
        LogHub::sink(&hub, LogLevel::Trace, __FILE__, __LINE__, "after the stop");
        hub.flush();
        hub.stop_writer();  // a second stop does nothing
        CHECK(hub.records() == static_cast<std::uint64_t>(kRecords) + 1);
    }
    const std::string all = read_text(path);
    const std::vector<int> late = numbers_after(all, ": late ");
    REQUIRE(late.size() == static_cast<std::size_t>(kRecords));
    bool ordered = true;
    for (int i = 0; i < kRecords; ++i) {
        ordered = ordered && late[static_cast<std::size_t>(i)] == i;
    }
    CHECK(ordered);
    const std::size_t after = all.find("after the stop");
    CHECK(after != std::string::npos);
    CHECK(after > all.find(": late " + std::to_string(kRecords - 1)));
    std::fclose(file);
}

// WP-58 (review, finding 6): sequence numbers and time stamps rise in the order of the file, whichever thread logs.
TEST_CASE("log hub: the sequence numbers of the file rise, with several threads logging") {
    SinkGuard guard;
    testpdb::TempDir dir;
    const std::string path = dir.file("log_seq.json");
    std::FILE* file = open_scratch(path);
    REQUIRE(file != nullptr);
    {
        LogHub::Options options;
        options.file = file;
        options.format = LogFormat::Json;
        options.background_writer = true;
        LogHub hub(options);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&hub] {
                for (int i = 0; i < 500; ++i) {
                    LogHub::sink(&hub, LogLevel::Debug, __FILE__, __LINE__, "x");
                }
            });
        }
        for (std::thread& t : threads) {
            t.join();
        }
    }
    const std::string all = read_text(path);
    std::int64_t last_seq = 0;
    std::int64_t last_t = 0;
    std::size_t lines = 0;
    bool rising = true;
    for (std::size_t at = 0; at < all.size();) {
        const std::size_t end = all.find('\n', at);
        REQUIRE(end != std::string::npos);
        Result<ipc::json::Value> v = ipc::json::parse(all.substr(at, end - at));
        at = end + 1;
        REQUIRE(v.has_value());
        const std::int64_t seq = v->get_integer("seq");
        const std::int64_t t = v->get_integer("t_ms");
        rising = rising && seq == last_seq + 1 && t >= last_t;
        last_seq = seq;
        last_t = t;
        ++lines;
    }
    CHECK(lines == 2000);
    CHECK(rising);
    std::fclose(file);
}

// WP-58 (review, finding 5): run_headless installs its hub with a LogSinkScope declared after it, so that an early return
// puts the default sink back before the hub is destroyed. A log call after the scope never reaches the hub.
TEST_CASE("log hub: a LogSinkScope puts the default sink back on an early return") {
    SinkGuard guard;
    set_log_level(LogLevel::Info);
    LogHub::Options options;
    LogHub hub(options);
    const auto startup = [&hub](bool fail_early) -> int {
        const LogSinkScope scope(hub);
        CML_LOG(Info, "starting");
        if (fail_early) {
            return 1;  // like run_headless when the pipe name cannot be resolved
        }
        CML_LOG(Info, "running");
        return 0;
    };
    CHECK(startup(true) == 1);
    CHECK(hub.records() == 1);
    CML_LOG(Info, "after the early return");  // to the default sink (stderr), not to the hub
    CHECK(hub.records() == 1);
    CHECK(startup(false) == 0);
    CHECK(hub.records() == 3);
    CML_LOG(Info, "after the normal return");
    CHECK(hub.records() == 3);
}
