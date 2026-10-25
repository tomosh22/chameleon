// The agent's log sink: what it forwards to the broker must always be well-formed UTF-8.
#if defined(_WIN32)

#include <string>
#include <vector>

#include "agent_log.h"
#include "cml/ipc/messages.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;

TEST_CASE("a log line cut by the C runtime inside a multi-byte character is forwarded as valid UTF-8") {
    agent::install_logging();
    set_log_level(LogLevel::Warn);
    agent::set_log_forwarding(true);
    agent::take_forwarded_logs(1000);  // start from an empty queue

    // cml::log_write formats into a 1024-byte buffer, so a 1022-byte prefix followed by a 3-byte
    // character is cut after the first byte of that character.
    const std::string message = std::string(1022, 'a') + "\xE2\x82\xAC tail";
    CML_LOG(Warn, "%s", message.c_str());

    const std::vector<ipc::Log> logs = agent::take_forwarded_logs(1000);
    agent::set_log_forwarding(false);
    set_log_level(LogLevel::Info);

    REQUIRE_FALSE(logs.empty());
    for (const ipc::Log& log : logs) {
        CHECK(ipc::is_valid_utf8(log.text));
        CHECK(ipc::encode_frame(log).has_value());
    }
}

TEST_CASE("only warnings and errors are forwarded, and only while someone wants them") {
    agent::install_logging();
    set_log_level(LogLevel::Debug);
    agent::take_forwarded_logs(1000);
    CML_LOG(Error, "nobody is listening");  // forwarding is off: dropped
    CHECK(agent::take_forwarded_logs(1000).empty());

    agent::set_log_forwarding(true);
    CML_LOG(Info, "too quiet");
    CML_LOG(Warn, "worth forwarding");
    const std::vector<ipc::Log> logs = agent::take_forwarded_logs(1000);
    agent::set_log_forwarding(false);
    set_log_level(LogLevel::Info);
    REQUIRE(logs.size() == 1);
    CHECK(logs[0].text.find("worth forwarding") != std::string::npos);
}

#endif  // _WIN32
