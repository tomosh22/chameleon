#include "cml/util/log.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"

namespace {

struct Captured {
    cml::LogLevel level;
    std::string file;
    int line;
    std::string message;
};

void capture(void* user, cml::LogLevel level, const char* file, int line, const char* message) {
    static_cast<std::vector<Captured>*>(user)->push_back({level, file, line, message});
}

struct SinkGuard {
    explicit SinkGuard(std::vector<Captured>* out) : saved_level(cml::log_level()) {
        cml::set_log_sink(&capture, out);
    }
    ~SinkGuard() {
        cml::set_log_sink(nullptr, nullptr);
        cml::set_log_level(saved_level);
    }
    cml::LogLevel saved_level;
};

}  // namespace

TEST_CASE("CML_LOG formats and delivers to the sink") {
    std::vector<Captured> got;
    SinkGuard guard(&got);
    cml::set_log_level(cml::LogLevel::Trace);

    CML_LOG(Info, "attached pid=%u name=%s", 1234u, "game.exe");

    REQUIRE(got.size() == 1);
    CHECK(got[0].level == cml::LogLevel::Info);
    CHECK(got[0].message == "attached pid=1234 name=game.exe");
    CHECK(got[0].line > 0);
    CHECK(got[0].file.find("test_log.cpp") != std::string::npos);
}

TEST_CASE("CML_LOG filters by level and does not evaluate arguments when disabled") {
    std::vector<Captured> got;
    SinkGuard guard(&got);
    cml::set_log_level(cml::LogLevel::Warn);

    int evaluated = 0;
    CML_LOG(Debug, "x=%d", ++evaluated);
    CHECK(evaluated == 0);
    CHECK(got.empty());

    CML_LOG(Error, "y=%d", ++evaluated);
    CHECK(evaluated == 1);
    REQUIRE(got.size() == 1);
    CHECK(got[0].message == "y=1");
}

TEST_CASE("Level Off silences everything") {
    std::vector<Captured> got;
    SinkGuard guard(&got);
    cml::set_log_level(cml::LogLevel::Off);
    CML_LOG(Error, "nope");
    CHECK(got.empty());
    CHECK_FALSE(cml::log_enabled(cml::LogLevel::Error));
}

TEST_CASE("Long messages are truncated, not overrun") {
    std::vector<Captured> got;
    SinkGuard guard(&got);
    cml::set_log_level(cml::LogLevel::Trace);
    const std::string big(4000, 'a');
    CML_LOG(Trace, "%s", big.c_str());
    REQUIRE(got.size() == 1);
    CHECK(got[0].message.size() == 1023u);
}

TEST_CASE("Level names") {
    CHECK(std::string(cml::to_string(cml::LogLevel::Trace)) == "trace");
    CHECK(std::string(cml::to_string(cml::LogLevel::Error)) == "error");
}

TEST_CASE("CML_LOG(Off, ...) never emits, whatever the threshold") {
    std::vector<Captured> got;
    SinkGuard guard(&got);
    cml::set_log_level(cml::LogLevel::Trace);
    CML_LOG(Off, "must not appear");
    CHECK(got.empty());
    CHECK_FALSE(cml::log_enabled(cml::LogLevel::Off));
    cml::set_log_level(cml::LogLevel::Off);
    CML_LOG(Off, "must not appear either");
    CHECK(got.empty());
}

namespace {

struct SinkContext {
    std::atomic<int> calls{0};
};
SinkContext g_ctx_a;
SinkContext g_ctx_b;
std::atomic<int> g_mismatches{0};

// Each sink only ever expects its own context. Pairing sink A with context B counts a mismatch.
void sink_a(void* user, cml::LogLevel, const char*, int, const char*) {
    if (user != &g_ctx_a) {
        g_mismatches.fetch_add(1);
    }
    g_ctx_a.calls.fetch_add(1);
}
void sink_b(void* user, cml::LogLevel, const char*, int, const char*) {
    if (user != &g_ctx_b) {
        g_mismatches.fetch_add(1);
    }
    g_ctx_b.calls.fetch_add(1);
}

}  // namespace

TEST_CASE("set_log_sink publishes sink and user together while other threads log") {
    const cml::LogLevel saved = cml::log_level();
    cml::set_log_level(cml::LogLevel::Trace);
    cml::set_log_sink(&sink_a, &g_ctx_a);
    g_mismatches = 0;

    std::atomic<bool> stop{false};
    std::vector<std::thread> loggers;
    for (int t = 0; t < 4; ++t) {
        loggers.emplace_back([&stop] {
            while (!stop.load()) {
                CML_LOG(Info, "tick");
            }
        });
    }
    // Keep swapping until the loggers have demonstrably run concurrently with the swaps; on a
    // loaded runner the threads may not be scheduled before a fixed number of swaps completes.
    for (int i = 0; i < 20000 || g_ctx_a.calls.load() + g_ctx_b.calls.load() < 1000; ++i) {
        if ((i & 1) == 0) {
            cml::set_log_sink(&sink_b, &g_ctx_b);
        } else {
            cml::set_log_sink(&sink_a, &g_ctx_a);
        }
    }
    stop = true;
    for (auto& th : loggers) {
        th.join();
    }

    CHECK(g_mismatches.load() == 0);
    CHECK(g_ctx_a.calls.load() + g_ctx_b.calls.load() > 0);

    cml::set_log_sink(nullptr, nullptr);
    cml::set_log_level(saved);
}
