// Control protocol codec (portable) and the broker's request handler, including malformed input.
#include <atomic>
#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "cml/core/control_server.h"
#include "cml/core/process_registry.h"
#include "cml/core/session.h"
#include "cml/ipc/control.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::ipc;

namespace {

ControlRequest request(std::int64_t id, const std::string& cmd) {
    ControlRequest r;
    r.id = id;
    r.cmd = cmd;
    return r;
}

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

json::Value parsed(const std::string& line) {
    Result<json::Value> r = json::parse(line);
    REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? "" : r.error().format()));
    return *r;
}

}  // namespace

TEST_CASE("control: request round trip") {
    ControlRequest in = request(7, "reload");
    in.args.set("timeout_ms", json::Value::integer(5000));
    Result<std::string> line = encode_request(in);
    REQUIRE(line.has_value());
    CHECK(line->find('\n') == std::string::npos);
    Result<ControlRequest> out = decode_request(*line);
    REQUIRE(out.has_value());
    CHECK(out->id == 7);
    CHECK(out->cmd == "reload");
    CHECK(out->args.get_integer("timeout_ms") == 5000);
}

TEST_CASE("control: args are optional and default to an empty object") {
    Result<ControlRequest> out = decode_request("{\"id\":1,\"cmd\":\"status\"}");
    REQUIRE(out.has_value());
    CHECK(out->args.is_object());
    CHECK(out->args.size() == 0);
    // unknown extra members are ignored (forward compatibility)
    CHECK(decode_request("{\"id\":1,\"cmd\":\"status\",\"future\":[1,2]}").has_value());
}

TEST_CASE("control: invalid requests give CML5400 (not JSON) or CML5401 (not a request)") {
    struct Case {
        const char* line;
        std::uint32_t code;
    };
    const std::vector<Case> cases = {
        {"", ids::kControlJsonMalformed},
        {"not json", ids::kControlJsonMalformed},
        {"{\"id\":1,\"cmd\":", ids::kControlJsonMalformed},
        {"[1,2,3]", ids::kControlRequestInvalid},
        {"\"status\"", ids::kControlRequestInvalid},
        {"null", ids::kControlRequestInvalid},
        {"{}", ids::kControlRequestInvalid},
        {"{\"cmd\":\"status\"}", ids::kControlRequestInvalid},
        {"{\"id\":\"1\",\"cmd\":\"status\"}", ids::kControlRequestInvalid},
        {"{\"id\":1.5,\"cmd\":\"status\"}", ids::kControlRequestInvalid},
        {"{\"id\":1}", ids::kControlRequestInvalid},
        {"{\"id\":1,\"cmd\":5}", ids::kControlRequestInvalid},
        {"{\"id\":1,\"cmd\":\"\"}", ids::kControlRequestInvalid},
        {"{\"id\":1,\"cmd\":\"Status\"}", ids::kControlRequestInvalid},
        {"{\"id\":1,\"cmd\":\"a b\"}", ids::kControlRequestInvalid},
        {"{\"id\":1,\"cmd\":\"status\",\"args\":[]}", ids::kControlRequestInvalid},
        {"{\"id\":1,\"cmd\":\"status\",\"args\":5}", ids::kControlRequestInvalid},
    };
    for (const Case& c : cases) {
        INFO("line: " << c.line);
        Result<ControlRequest> r = decode_request(c.line);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == c.code);
    }
    const std::string long_name(65, 'a');
    CHECK_FALSE(decode_request("{\"id\":1,\"cmd\":\"" + long_name + "\"}").has_value());
    // a line above the limit is refused before parsing
    const std::string huge(kMaxControlLineBytes + 1, ' ');
    Result<ControlRequest> too_big = decode_request(huge);
    REQUIRE_FALSE(too_big.has_value());
    CHECK(too_big.error().code == ids::kControlLineTooLong);
}

TEST_CASE("control: ids may be negative or large, and are echoed") {
    Result<ControlRequest> r = decode_request("{\"id\":-9007199254740993,\"cmd\":\"status\"}");
    REQUIRE(r.has_value());
    CHECK(r->id == -9007199254740993ll);
    CHECK(peek_control_request_id("{\"id\":42,\"cmd\":5}") == std::optional<std::int64_t>(42));
    CHECK_FALSE(peek_control_request_id("garbage").has_value());
    CHECK_FALSE(peek_control_request_id("[1]").has_value());
    CHECK_FALSE(peek_control_request_id("{\"id\":\"x\"}").has_value());
}

TEST_CASE("control: response round trip, ok and error") {
    json::Value result = json::Value::object();
    result.set("generation", json::Value::integer(3));
    Result<std::string> ok_line = encode_response(make_ok_response(5, result));
    REQUIRE(ok_line.has_value());
    Result<std::variant<ControlResponse, ControlEvent>> decoded = decode_server_message(*ok_line);
    REQUIRE(decoded.has_value());
    REQUIRE(std::holds_alternative<ControlResponse>(*decoded));
    const ControlResponse& ok = std::get<ControlResponse>(*decoded);
    CHECK(ok.ok);
    CHECK(ok.id == std::optional<std::int64_t>(5));
    CHECK(ok.result.get_integer("generation") == 3);
    CHECK_FALSE(ok.diag.has_value());

    const Diag diag = Diag::make(ids::kControlUnknownCommand, "no such command", "use status");
    Result<std::string> err_line = encode_response(make_error_response(9, diag));
    REQUIRE(err_line.has_value());
    const json::Value err_json = parsed(*err_line);
    CHECK(err_json.get_bool("ok", true) == false);
    REQUIRE(err_json.find("diag") != nullptr);
    CHECK(err_json.find("diag")->get_string("id") == "CML5402");
    CHECK(err_json.find("diag")->get_string("message") == "no such command");
    CHECK(err_json.find("diag")->get_string("action") == "use status");
    Result<std::variant<ControlResponse, ControlEvent>> back = decode_server_message(*err_line);
    REQUIRE(back.has_value());
    const ControlResponse& err = std::get<ControlResponse>(*back);
    CHECK_FALSE(err.ok);
    REQUIRE(err.diag.has_value());
    const Diag round = to_diag(*err.diag);
    CHECK(round.code == ids::kControlUnknownCommand);
    CHECK(round.message == "no such command");

    // an unreadable request id is answered with "id":null
    Result<std::string> null_id = encode_response(make_error_response(std::nullopt, diag));
    REQUIRE(null_id.has_value());
    CHECK(null_id->find("\"id\":null") != std::string::npos);
    Result<std::variant<ControlResponse, ControlEvent>> null_back = decode_server_message(*null_id);
    REQUIRE(null_back.has_value());
    CHECK_FALSE(std::get<ControlResponse>(*null_back).id.has_value());
}

TEST_CASE("control: events") {
    ControlEvent event;
    event.name = "diag";
    event.body.set("id", json::Value::string("CML3101"));
    event.body.set("message", json::Value::string("x"));
    Result<std::string> line = encode_event(event);
    REQUIRE(line.has_value());
    Result<std::variant<ControlResponse, ControlEvent>> decoded = decode_server_message(*line);
    REQUIRE(decoded.has_value());
    REQUIRE(std::holds_alternative<ControlEvent>(*decoded));
    const ControlEvent& out = std::get<ControlEvent>(*decoded);
    CHECK(out.name == "diag");
    CHECK(out.body.get_string("id") == "CML3101");
    CHECK(out.body.get_string("event") == "diag");
}

TEST_CASE("control: malformed messages from the broker are CML5407") {
    const std::vector<std::string> bad = {
        "garbage",
        "[]",
        "{}",
        "{\"id\":1}",
        "{\"id\":1,\"ok\":\"yes\"}",
        "{\"id\":\"1\",\"ok\":true}",
        "{\"id\":1,\"ok\":true,\"result\":5}",
        "{\"id\":1,\"ok\":false}",
        "{\"id\":1,\"ok\":false,\"diag\":{\"id\":\"CML5402\"}}",
        "{\"id\":1,\"ok\":false,\"diag\":\"text\"}",
        "{\"event\":5}",
        "{\"event\":\"\"}",
    };
    for (const std::string& line : bad) {
        INFO("line: " << line);
        Result<std::variant<ControlResponse, ControlEvent>> r = decode_server_message(line);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ids::kControlResponseInvalid);
    }
}

TEST_CASE("control: a diag with an unusable id still maps to a valid Diag") {
    ControlDiag d;
    d.id = "oops";
    d.message = "m";
    d.action = "a";
    const Diag diag = to_diag(d);
    CHECK(diag.valid());
    CHECK(diag.code == ids::kControlResponseInvalid);
    CHECK(diag.message.find("oops") != std::string::npos);
}

TEST_CASE("control: oversized messages are refused by the encoders") {
    ControlRequest r = request(1, "status");
    r.args.set("blob", json::Value::string(std::string(json::kMaxStringBytes, 'x')));
    for (int i = 0; i < 6; ++i) {
        r.args.set("blob" + std::to_string(i), json::Value::string(std::string(json::kMaxStringBytes, 'y')));
    }
    Result<std::string> line = encode_request(r);
    REQUIRE_FALSE(line.has_value());
    CHECK(line.error().code == ids::kControlLineTooLong);
}

TEST_CASE("control: status report round trip and lookup") {
    StatusReport report;
    report.version = "1.2.3";
    report.capabilities = {"status", "reload"};
    StatusProcess p;
    p.pid = 4321;
    p.modules.push_back({"C:\\x\\game.exe", true});
    p.modules.push_back({"C:\\Windows\\kernel32.dll", false});
    report.processes.push_back(p);

    Result<StatusReport> back = status_from_json(status_to_json(report));
    REQUIRE(back.has_value());
    CHECK(back->version == "1.2.3");
    CHECK(back->has_capability("reload"));
    CHECK_FALSE(back->has_capability("wait"));
    REQUIRE(back->processes.size() == 1);
    CHECK(back->find_process(4321) != nullptr);
    CHECK(back->find_process(1) == nullptr);
    CHECK(back->processes[0].modules[0].enabled);
    CHECK_FALSE(back->processes[0].modules[1].enabled);
    CHECK(back->processes[0].modules[0].path == "C:\\x\\game.exe");

    const std::vector<std::string> bad = {
        "[]",
        "{}",
        "{\"capabilities\":[],\"processes\":5}",
        "{\"capabilities\":[1],\"processes\":[]}",
        "{\"capabilities\":[],\"processes\":[{\"pid\":-1,\"modules\":[]}]}",
        "{\"capabilities\":[],\"processes\":[{\"pid\":1}]}",
        "{\"capabilities\":[],\"processes\":[{\"pid\":1,\"modules\":[{\"path\":\"x\"}]}]}",
        "{\"capabilities\":[],\"processes\":[{\"pid\":1,\"modules\":[{\"path\":1,\"enabled\":true}]}]}",
    };
    for (const std::string& text : bad) {
        INFO("status: " << text);
        CHECK_FALSE(status_from_json(parsed(text)).has_value());
    }
}

// --- broker handler ------------------------------------------------------------------------

namespace {

core::ControlContext context_with(core::ProcessRegistry* registry) {
    core::ControlContext c;
    c.version = "9.9.9";
    c.registry = registry;
    return c;
}

ControlResponse respond(const std::string& line, const core::ControlContext& context) {
    const std::string answer = core::handle_control_line(line, context);
    Result<std::variant<ControlResponse, ControlEvent>> decoded = decode_server_message(answer);
    REQUIRE_MESSAGE(decoded.has_value(), (decoded.has_value() ? "" : decoded.error().format()));
    REQUIRE(std::holds_alternative<ControlResponse>(*decoded));
    return std::get<ControlResponse>(*decoded);
}

}  // namespace

TEST_CASE("broker control: status with no processes") {
    core::ProcessRegistry registry;
    const ControlResponse r = respond("{\"id\":1,\"cmd\":\"status\"}", context_with(&registry));
    REQUIRE(r.ok);
    CHECK(r.id == std::optional<std::int64_t>(1));
    Result<StatusReport> report = status_from_json(r.result);
    REQUIRE(report.has_value());
    CHECK(report->version == "9.9.9");
    CHECK(report->capabilities == std::vector<std::string>{"status"});
    CHECK(report->processes.empty());
}

TEST_CASE("broker control: status lists registered processes and enabled modules") {
    core::ProcessRegistry registry;
    QuietLogs quiet;
    core::Session session(1, "9.9.9");
    ipc::Hello hello;
    hello.request_id = 1;
    hello.pid = 777;
    hello.agent_path = "C:\\agent\\cml_agent.dll";
    session.on_message(hello);
    ipc::ModuleList list;
    list.request_id = 2;
    list.modules.push_back({0x140000000ull, 0x30000, "C:\\Games\\game.exe"});
    list.modules.push_back({0x180000000ull, 0x10000, "C:\\Games\\other.dll"});
    session.on_message(list);
    ipc::EnableModule enable;
    enable.request_id = 3;
    enable.path = "C:\\Games\\game.exe";
    session.on_message(enable);
    registry.update(session);

    const ControlResponse r = respond("{\"id\":2,\"cmd\":\"status\",\"args\":{}}", context_with(&registry));
    REQUIRE(r.ok);
    Result<StatusReport> report = status_from_json(r.result);
    REQUIRE(report.has_value());
    REQUIRE(report->processes.size() == 1);
    const StatusProcess& p = report->processes[0];
    CHECK(p.pid == 777);
    REQUIRE(p.modules.size() == 2);
    CHECK(p.modules[0].path == "C:\\Games\\game.exe");  // enabled modules come first
    CHECK(p.modules[0].enabled);
    CHECK(p.modules[1].path == "C:\\Games\\other.dll");
    CHECK_FALSE(p.modules[1].enabled);

    registry.remove(1);
    const ControlResponse after = respond("{\"id\":3,\"cmd\":\"status\"}", context_with(&registry));
    CHECK(status_from_json(after.result)->processes.empty());
}

TEST_CASE("broker control: a session that has not said Hello is not listed") {
    core::ProcessRegistry registry;
    QuietLogs quiet;
    core::Session session(1, "9.9.9");
    registry.update(session);
    CHECK(registry.snapshot().empty());
}

TEST_CASE("broker control: status truncates huge module lists, enabled first") {
    core::ProcessRegistry registry;
    QuietLogs quiet;
    core::Session session(1, "9.9.9");
    ipc::Hello hello;
    hello.request_id = 1;
    hello.pid = 5;
    session.on_message(hello);
    ipc::ModuleList list;
    list.request_id = 2;
    for (std::size_t i = 0; i < core::kMaxStatusModulesPerProcess + 50; ++i) {
        list.modules.push_back({0x1000ull * (i + 1), 0x1000, "C:\\m\\mod" + std::to_string(i) + ".dll"});
    }
    session.on_message(list);
    ipc::EnableModule enable;
    enable.request_id = 3;
    enable.path = "C:\\m\\mod" + std::to_string(core::kMaxStatusModulesPerProcess + 10) + ".dll";
    session.on_message(enable);
    registry.update(session);
    const ControlResponse r = respond("{\"id\":1,\"cmd\":\"status\"}", context_with(&registry));
    REQUIRE(r.ok);
    CHECK(r.result.get_bool("truncated"));
    Result<StatusReport> report = status_from_json(r.result);
    REQUIRE(report.has_value());
    CHECK(report->truncated);
    CHECK(report->processes[0].modules_truncated);
    CHECK(report->processes[0].modules.size() == core::kMaxStatusModulesPerProcess);
    CHECK(report->processes[0].modules[0].enabled);  // the enabled one survived the cut
}

TEST_CASE("broker control: every other command is a correct 'unknown command' diag") {
    core::ProcessRegistry registry;
    for (const char* cmd : {"reload", "wait", "set-option", "subscribe", "nonsense"}) {
        INFO("cmd: " << cmd);
        const ControlResponse r = respond(std::string("{\"id\":11,\"cmd\":\"") + cmd + "\",\"args\":{}}", context_with(&registry));
        CHECK_FALSE(r.ok);
        CHECK(r.id == std::optional<std::int64_t>(11));
        REQUIRE(r.diag.has_value());
        CHECK(r.diag->id == "CML5402");
        CHECK(r.diag->message.find(cmd) != std::string::npos);
        CHECK(r.diag->message.find("status") != std::string::npos);  // names what IS provided
        CHECK_FALSE(r.diag->action.empty());
    }
}

TEST_CASE("broker control: malformed lines get a diag response and the id when readable") {
    core::ProcessRegistry registry;
    const core::ControlContext context = context_with(&registry);
    {
        const ControlResponse r = respond("this is not json", context);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.id.has_value());
        REQUIRE(r.diag.has_value());
        CHECK(r.diag->id == "CML5400");
    }
    {
        const ControlResponse r = respond("{\"id\":8,\"cmd\":42}", context);
        CHECK_FALSE(r.ok);
        CHECK(r.id == std::optional<std::int64_t>(8));
        REQUIRE(r.diag.has_value());
        CHECK(r.diag->id == "CML5401");
    }
    {
        const ControlResponse r = respond("[1,2]", context);
        REQUIRE(r.diag.has_value());
        CHECK(r.diag->id == "CML5401");
    }
    // and the handler still works afterwards (no state is corrupted)
    CHECK(respond("{\"id\":9,\"cmd\":\"status\"}", context).ok);
}

TEST_CASE("broker control: handler without a registry still answers status") {
    core::ControlContext context;
    context.version = "1";
    const ControlResponse r = respond("{\"id\":1,\"cmd\":\"status\"}", context);
    REQUIRE(r.ok);
    CHECK(status_from_json(r.result)->processes.empty());
}

TEST_CASE("broker control: status stays under 75% of the line limit with many large modules (review fix 3)") {
    QuietLogs quiet;
    core::ProcessRegistry registry;
    core::Session first(1, "9.9.9");
    core::Session second(2, "9.9.9");
    core::Session third(3, "9.9.9");
    core::Session* sessions[] = {&first, &second, &third};
    std::uint32_t pid = 100;
    for (core::Session* session : sessions) {
        ipc::Hello hello;
        hello.request_id = 1;
        hello.pid = pid++;
        session->on_message(hello);
        ipc::ModuleList list;
        list.request_id = 2;
        for (std::size_t i = 0; i < 1900; ++i) {
            // ~600 byte paths, some with characters that JSON escapes (backslashes double)
            list.modules.push_back({0x1000ull * (i + 1), 0x1000, "C:\\very\\long\\directory\\" + std::string(560, 'x') + std::to_string(i) + ".dll"});
        }
        session->on_message(list);
        registry.update(*session);
    }
    const std::string answer = core::handle_control_line("{\"id\":5,\"cmd\":\"status\"}", context_with(&registry));
    CHECK(answer.size() <= core::kStatusByteBudget + 4096);
    CHECK(answer.size() < ipc::kMaxControlLineBytes);
    Result<std::variant<ControlResponse, ControlEvent>> decoded = decode_server_message(answer);
    REQUIRE(decoded.has_value());
    const ControlResponse& r = std::get<ControlResponse>(*decoded);
    REQUIRE_MESSAGE(r.ok, "status must never be replaced by an error");
    CHECK(r.result.get_bool("truncated"));
    Result<StatusReport> report = status_from_json(r.result);
    REQUIRE(report.has_value());
    REQUIRE_FALSE(report->processes.empty());
    CHECK(report->processes[0].modules_truncated);
    CHECK_FALSE(report->processes[0].modules.empty());
    // later processes are either listed (possibly without modules) or left out, never invented
    CHECK(report->processes.size() <= 3);

    // a single module whose path alone exceeds the budget is dropped, status still succeeds
    core::ProcessRegistry huge_registry;
    core::Session huge(9, "9.9.9");
    ipc::Hello hello;
    hello.request_id = 1;
    hello.pid = 777;
    huge.on_message(hello);
    ipc::ModuleList huge_list;
    huge_list.request_id = 2;
    huge_list.modules.push_back({0x1000ull, 0x1000, "C:\\" + std::string(900 * 1024, 'y')});
    huge_list.modules.push_back({0x2000ull, 0x1000, "C:\\small.dll"});
    huge.on_message(huge_list);
    huge_registry.update(huge);
    const ControlResponse huge_response = respond("{\"id\":6,\"cmd\":\"status\"}", context_with(&huge_registry));
    REQUIRE(huge_response.ok);
    CHECK(huge_response.result.get_bool("truncated"));
    Result<StatusReport> huge_report = status_from_json(huge_response.result);
    REQUIRE(huge_report.has_value());
    CHECK(huge_report->processes.size() == 1);
    CHECK(huge_report->processes[0].pid == 777);
    CHECK(huge_report->processes[0].modules_truncated);

    // an untruncated status carries no "truncated" member
    core::ProcessRegistry empty;
    const ControlResponse plain = respond("{\"id\":7,\"cmd\":\"status\"}", context_with(&empty));
    CHECK(plain.result.find("truncated") == nullptr);
    CHECK(plain.result.get_bool("control_pipe_healthy", false));
}

// --- review fix 9: the registry is rebuilt only when something it shows changed ---------------

TEST_CASE("process registry: rebuilt only when the module list or the enabled set changes (review fix 9)") {
    QuietLogs quiet;
    core::ProcessRegistry registry;
    core::Session session(1, "9.9.9");
    CHECK_FALSE(registry.update_if_changed(session));  // no handshake yet
    CHECK(registry.rebuilds() == 0);

    ipc::Hello hello;
    hello.request_id = 1;
    hello.pid = 321;
    session.on_message(hello);
    CHECK(registry.update_if_changed(session));
    CHECK_FALSE(registry.update_if_changed(session));
    CHECK(registry.rebuilds() == 1);

    ipc::Log log;
    log.level = 2;
    log.text = "noise";
    for (int i = 0; i < 100; ++i) {
        session.on_message(log);  // traffic that changes nothing the status shows
        CHECK_FALSE(registry.update_if_changed(session));
    }
    CHECK(registry.rebuilds() == 1);

    ipc::ModuleList list;
    list.request_id = 2;
    list.modules.push_back({0x1000ull, 0x1000, "C:\\a\\one.dll"});
    list.modules.push_back({0x2000ull, 0x1000, "C:\\a\\two.dll"});
    session.on_message(list);
    CHECK(registry.update_if_changed(session));
    CHECK(registry.rebuilds() == 2);

    ipc::EnableModule enable;
    enable.request_id = 3;
    enable.path = "C:\\a\\one.dll";
    session.on_message(enable);
    CHECK(registry.update_if_changed(session));
    session.on_message(enable);  // already enabled: nothing changes
    CHECK_FALSE(registry.update_if_changed(session));
    CHECK(registry.rebuilds() == 3);

    ipc::EnableModule refused;
    refused.request_id = 4;
    refused.path = "C:\\a\\missing.dll";
    session.on_message(refused);  // rejected request: nothing changes
    CHECK_FALSE(registry.update_if_changed(session));

    ipc::ModuleLoaded loaded;
    loaded.module = {0x3000ull, 0x1000, "C:\\a\\three.dll"};
    session.on_message(loaded);
    CHECK(registry.update_if_changed(session));
    ipc::ModuleUnloaded unloaded;
    unloaded.module = {0x3000ull, 0x1000, "C:\\a\\three.dll"};
    session.on_message(unloaded);
    CHECK(registry.update_if_changed(session));

    ipc::DisableModule disable;
    disable.request_id = 5;
    disable.path = "C:\\a\\one.dll";
    session.on_message(disable);
    CHECK(registry.update_if_changed(session));
    CHECK(registry.rebuilds() == 6);

    const std::vector<core::RegisteredProcess> snapshot = registry.snapshot();
    REQUIRE(snapshot.size() == 1);
    CHECK(snapshot[0].pid == 321);
    REQUIRE(snapshot[0].modules.size() == 2);
    CHECK_FALSE(snapshot[0].modules[0].enabled);
}

TEST_CASE("process registry: enabled matching uses normalised paths and scales") {
    CHECK(core::normalized_path_key("C:/Games/GAME.exe") == core::normalized_path_key("c:\\games\\game.EXE"));
    CHECK(core::normalized_path_key("\\\\?\\C:\\x.dll") == core::normalized_path_key("c:\\X.DLL"));
    CHECK(core::normalized_path_key("C:\\x.dll") != core::normalized_path_key("C:\\y.dll"));
    CHECK(core::normalized_path_key("").empty());

    QuietLogs quiet;
    core::Session session(1, "9.9.9");
    ipc::Hello hello;
    hello.request_id = 1;
    hello.pid = 9;
    session.on_message(hello);
    ipc::ModuleList list;
    list.request_id = 2;
    constexpr std::size_t kModules = 8000;
    for (std::size_t i = 0; i < kModules; ++i) {
        list.modules.push_back({0x1000ull * (i + 1), 0x1000, "C:\\mods\\Mod" + std::to_string(i) + ".dll"});
    }
    session.on_message(list);
    for (std::size_t i = 0; i < kModules; i += 2) {
        ipc::EnableModule enable;
        enable.request_id = 10 + i;
        enable.path = "c:/MODS/mod" + std::to_string(i) + ".DLL";  // different spelling than the list
        session.on_message(enable);
    }
    REQUIRE(session.enabled_modules().size() == kModules / 2);
    core::ProcessRegistry registry;
    const auto start = std::chrono::steady_clock::now();
    registry.update(session);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms < 500);
    const std::vector<core::RegisteredProcess> snapshot = registry.snapshot();
    REQUIRE(snapshot.size() == 1);
    std::size_t enabled = 0;
    for (const core::RegisteredModule& m : snapshot[0].modules) {
        enabled += m.enabled ? 1 : 0;
    }
    CHECK(enabled == kModules / 2);
    CHECK(snapshot[0].modules[0].enabled);
    CHECK_FALSE(snapshot[0].modules[1].enabled);
}

TEST_CASE("broker control: status reports control_pipe_healthy (review fix 8)") {
    core::ProcessRegistry registry;
    std::atomic<bool> healthy{true};
    core::ControlContext context = context_with(&registry);
    context.pipe_healthy = &healthy;
    CHECK(respond("{\"id\":1,\"cmd\":\"status\"}", context).result.get_bool("control_pipe_healthy", false));
    healthy.store(false);
    const ControlResponse r = respond("{\"id\":2,\"cmd\":\"status\"}", context);
    REQUIRE(r.ok);
    REQUIRE(r.result.find("control_pipe_healthy") != nullptr);
    CHECK_FALSE(r.result.get_bool("control_pipe_healthy", true));
    Result<StatusReport> report = status_from_json(r.result);
    REQUIRE(report.has_value());
    CHECK_FALSE(report->control_pipe_healthy);
}
