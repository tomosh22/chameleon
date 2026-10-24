#include "cml/util/log.h"

#include <string>
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
