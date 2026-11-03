// ExceptionService (WP-20): the broker side of crash -> patch -> continue, against a scripted fake agent. Portable.
#include "cml/core/exception_service.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <variant>
#include <vector>

#include "cml/core/control_server.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
using ipc::json::Value;

namespace {

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

struct Collect final : EventListener {
    void on_event(const Event& event) override {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(event);
    }
    std::size_t count(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.name == name; }));
    }
    std::mutex mutex;
    std::vector<Event> events;
};

// A broker with one connected "agent": a link whose transport records every message and answers the requests like the real agent
// would (Reply status chosen by the test).
struct Rig {
    Rig() : service(PipelineEnv{}, &bus, ""), exceptions(&service, &bus) {
        link = std::make_shared<AgentLink>(5, [this](const ipc::Message& message) -> Result<void> {
            {
                std::lock_guard<std::mutex> lock(mutex);
                sent.push_back(message);
            }
            const std::optional<std::uint64_t> id = request_id_of(message);
            if (id && (std::holds_alternative<ipc::ExceptionConfig>(message) || std::holds_alternative<ipc::ExceptionAction>(message))) {
                std::uint32_t status = 0;
                if (std::holds_alternative<ipc::ExceptionAction>(message)) {
                    status = action_status;
                }
                std::lock_guard<std::mutex> lock(mutex);
                answerers.emplace_back([this, id = *id, status] {
                    ipc::Reply reply;
                    reply.request_id = id;
                    reply.status = status;
                    reply.message = status == 0 ? "ok" : "the agent says no";
                    link->deliver(reply);
                });
            }
            return Result<void>();
        });
        exceptions.agent_ready(link);
    }
    ~Rig() {
        for (std::thread& t : answerers) {
            t.join();
        }
    }

    // A control client that listens for `exception` events (what `subscribe` does).
    EventBus::Id listen(std::shared_ptr<Collect> collector, std::set<std::string> names = {"exception"}) {
        const EventBus::Id id = bus.subscribe(std::move(collector), std::move(names));
        exceptions.refresh(true);
        return id;
    }
    void stop_listening(EventBus::Id id) {
        bus.unsubscribe(id);
        exceptions.refresh(true);
    }

    std::vector<ipc::ExceptionConfig> configs() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<ipc::ExceptionConfig> out;
        for (const ipc::Message& m : sent) {
            if (const auto* c = std::get_if<ipc::ExceptionConfig>(&m)) {
                out.push_back(*c);
            }
        }
        return out;
    }
    std::vector<ipc::ExceptionAction> actions() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<ipc::ExceptionAction> out;
        for (const ipc::Message& m : sent) {
            if (const auto* a = std::get_if<ipc::ExceptionAction>(&m)) {
                out.push_back(*a);
            }
        }
        return out;
    }

    static ipc::Exception fault(std::uint32_t id) {
        ipc::Exception e;
        e.exception_id = id;
        e.thread_id = 1234;
        e.code = 0xC0000005u;
        e.access = 1;
        e.address = 0x00007FF600001040ull;
        e.data_address = 0;
        e.stack_pointer = 0x000000A0B0C0D0E0ull;
        e.module_base = 0x00007FF600000000ull;
        e.module_path = "C:\\build\\fixture_crash.exe";
        return e;
    }

    static Value args(std::int64_t pid, std::int64_t id, const char* action) {
        Value a = Value::object();
        a.set("pid", Value::integer(pid));
        a.set("exception_id", Value::integer(id));
        a.set("action", Value::string(action));
        return a;
    }

    EventBus bus;
    ReloadService service;
    ExceptionService exceptions;
    std::shared_ptr<AgentLink> link;
    std::mutex mutex;
    std::vector<ipc::Message> sent;
    std::vector<std::thread> answerers;
    std::atomic<std::uint32_t> action_status{0};
};

}  // namespace

TEST_CASE("exceptions: without a listener nothing is held and a fault that arrives is released as Ignore") {
    QuietLogs quiet;
    Rig rig;
    CHECK_FALSE(rig.exceptions.active());
    CHECK(rig.configs().empty());  // the agent starts with holding off; no need to tell it
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(1));
    CHECK(rig.exceptions.pending_count() == 0);
    const auto actions = rig.actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].exception_id == 1);
    CHECK(actions[0].action == ipc::ExceptionActionKind::Ignore);
}

TEST_CASE("exceptions: a listener turns holding on; the event has the ADR-0032 shape; the commands see the thread") {
    QuietLogs quiet;
    Rig rig;
    auto collector = std::make_shared<Collect>();
    rig.listen(collector);
    CHECK(rig.exceptions.active());
    const auto configs = rig.configs();
    REQUIRE(configs.size() == 1);
    CHECK((configs[0].flags & ipc::kExceptionConfigEnabled) != 0);
    CHECK((configs[0].flags & ipc::kExceptionConfigBreakWithDebugger) == 0);

    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(7));
    REQUIRE(collector->count("exception") == 1);
    const Value& body = collector->events[0].body;
    CHECK(body.get_string("event") == "exception");
    CHECK(body.get_integer("pid") == 4242);
    CHECK(body.get_integer("tid") == 1234);
    CHECK(body.get_integer("exception_id") == 7);
    CHECK(body.get_string("code") == "0xC0000005");
    CHECK(body.get_string("address") == "0x00007FF600001040");
    CHECK(body.get_string("module") == "fixture_crash.exe");
    CHECK(body.find("function") != nullptr);
    CHECK(body.find("offset") != nullptr);
    CHECK(body.get_integer("offset") == 0x1040);  // no PDB index in this rig: the offset is inside the module
    CHECK(body.get_string("access") == "write");
    REQUIRE(body.find("actions") != nullptr);
    std::vector<std::string> names;
    for (const Value& a : body.find("actions")->items()) {
        names.push_back(a.as_string());
    }
    CHECK(names == std::vector<std::string>{"continue", "leave", "ignore"});

    Result<Value> pending = rig.exceptions.pending(Value::object());
    REQUIRE(pending.has_value());
    REQUIRE(pending->find("pending") != nullptr);
    REQUIRE(pending->find("pending")->items().size() == 1);
    CHECK(pending->find("pending")->items()[0].get_integer("exception_id") == 7);
    Value other_pid = Value::object();
    other_pid.set("pid", Value::integer(1));
    CHECK(rig.exceptions.pending(other_pid)->find("pending")->items().empty());
}

TEST_CASE("exceptions: exception-action sends the action to the agent and removes the thread once it was released") {
    QuietLogs quiet;
    Rig rig;
    rig.listen(std::make_shared<Collect>());
    for (const char* name : {"continue", "leave", "ignore"}) {
        const std::uint32_t id = name[0] == 'c' ? 1 : name[0] == 'l' ? 2 : 3;
        rig.exceptions.on_exception(rig.link, 4242, Rig::fault(id));
        Result<Value> result = rig.exceptions.action(Rig::args(4242, id, name));
        REQUIRE_MESSAGE(result.has_value(), name);
        CHECK(result->get_integer("pid") == 4242);
        CHECK(result->get_integer("exception_id") == id);
        CHECK(result->get_string("action") == name);
    }
    const auto actions = rig.actions();
    REQUIRE(actions.size() == 3);
    CHECK(actions[0].action == ipc::ExceptionActionKind::Continue);
    CHECK(actions[1].action == ipc::ExceptionActionKind::Leave);
    CHECK(actions[2].action == ipc::ExceptionActionKind::Ignore);
    CHECK(rig.exceptions.pending_count() == 0);
}

TEST_CASE("exceptions: a refusal by the agent keeps the thread pending; unknown ids and bad arguments are diagnosed") {
    QuietLogs quiet;
    Rig rig;
    rig.listen(std::make_shared<Collect>());
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(9));

    rig.action_status = 4401;  // "cannot unwind this frame"
    Result<Value> refused = rig.exceptions.action(Rig::args(4242, 9, "leave"));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().id() == "CML4401");
    CHECK(rig.exceptions.pending_count() == 1);  // the client may choose another action

    rig.action_status = 4400;  // "no such thread": it is gone
    Result<Value> gone = rig.exceptions.action(Rig::args(4242, 9, "continue"));
    REQUIRE_FALSE(gone.has_value());
    CHECK(gone.error().id() == "CML4400");
    CHECK(rig.exceptions.pending_count() == 0);

    Result<Value> unknown = rig.exceptions.action(Rig::args(4242, 77, "continue"));
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().id() == "CML5751");
    Result<Value> wrong_pid = rig.exceptions.action(Rig::args(1, 9, "continue"));
    REQUIRE_FALSE(wrong_pid.has_value());
    CHECK(wrong_pid.error().id() == "CML5751");

    Value missing = Value::object();
    CHECK(rig.exceptions.action(missing).error().id() == "CML5750");
    CHECK(rig.exceptions.action(Rig::args(4242, 9, "explode")).error().id() == "CML5750");
    Value no_action = Value::object();
    no_action.set("exception_id", Value::integer(9));
    CHECK(rig.exceptions.action(no_action).error().id() == "CML5750");
}

TEST_CASE("exceptions: the pid may be left out when the exception id is unambiguous") {
    QuietLogs quiet;
    Rig rig;
    rig.listen(std::make_shared<Collect>());
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(3));
    Value args = Value::object();
    args.set("exception_id", Value::integer(3));
    args.set("action", Value::string("continue"));
    Result<Value> result = rig.exceptions.action(args);
    REQUIRE(result.has_value());
    CHECK(result->get_integer("pid") == 4242);
}

TEST_CASE("exceptions: the last listener leaving, or the option turned off, switches holding off and clears the pending list") {
    QuietLogs quiet;
    Rig rig;
    auto collector = std::make_shared<Collect>();
    const EventBus::Id subscription = rig.listen(collector);
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(1));
    REQUIRE(rig.exceptions.pending_count() == 1);

    rig.stop_listening(subscription);
    CHECK_FALSE(rig.exceptions.active());
    CHECK(rig.exceptions.pending_count() == 0);
    auto configs = rig.configs();
    REQUIRE(configs.size() == 2);
    CHECK((configs[1].flags & ipc::kExceptionConfigEnabled) == 0);

    // Listening again turns it on; the option crash_handler = off keeps it off even with a listener.
    rig.listen(collector);
    CHECK(rig.exceptions.active());
    Value off = Value::object();
    off.set("name", Value::string("crash_handler"));
    off.set("value", Value::boolean(false));
    rig.service.set_option_observer([&rig](const std::string&) { rig.exceptions.refresh(true); });
    REQUIRE(rig.service.set_option(off).has_value());
    CHECK_FALSE(rig.exceptions.active());
    configs = rig.configs();
    CHECK((configs.back().flags & ipc::kExceptionConfigEnabled) == 0);

    // break_with_debugger travels with the enabling.
    Value on = Value::object();
    on.set("name", Value::string("crash_handler"));
    on.set("value", Value::boolean(true));
    REQUIRE(rig.service.set_option(on).has_value());
    Value dbg = Value::object();
    dbg.set("name", Value::string("break_with_debugger"));
    dbg.set("value", Value::boolean(true));
    REQUIRE(rig.service.set_option(dbg).has_value());
    configs = rig.configs();
    CHECK((configs.back().flags & ipc::kExceptionConfigEnabled) != 0);
    CHECK((configs.back().flags & ipc::kExceptionConfigBreakWithDebugger) != 0);

    Value bad = Value::object();
    bad.set("name", Value::string("crash_handler"));
    bad.set("value", Value::integer(3));
    CHECK_FALSE(rig.service.set_option(bad).has_value());
    rig.service.set_option_observer(nullptr);
}

TEST_CASE("exceptions: an agent that connects while holding is on is told; a gone agent's threads are forgotten") {
    QuietLogs quiet;
    Rig rig;
    rig.listen(std::make_shared<Collect>());
    std::vector<ipc::Message> late_sent;
    auto late = std::make_shared<AgentLink>(6, [&](const ipc::Message& m) -> Result<void> {
        late_sent.push_back(m);
        return Result<void>();
    });
    rig.exceptions.agent_ready(late);
    REQUIRE(late_sent.size() == 1);
    const auto* config = std::get_if<ipc::ExceptionConfig>(&late_sent[0]);
    REQUIRE(config != nullptr);
    CHECK((config->flags & ipc::kExceptionConfigEnabled) != 0);

    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(1));
    rig.exceptions.on_exception(late, 5555, Rig::fault(1));  // same exception id, other agent
    CHECK(rig.exceptions.pending_count() == 2);
    rig.exceptions.agent_gone(5);
    CHECK(rig.exceptions.pending_count() == 1);
    Value only = Value::object();
    only.set("pid", Value::integer(5555));
    CHECK(rig.exceptions.pending(only)->find("pending")->items().size() == 1);
}

TEST_CASE("exceptions: a request to a connection that is gone drops the thread and says why") {
    QuietLogs quiet;
    Rig rig;
    rig.listen(std::make_shared<Collect>());
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(2));
    rig.link->close();
    Result<Value> result = rig.exceptions.action(Rig::args(4242, 2, "continue"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().id() == "CML5752");
    CHECK(rig.exceptions.pending_count() == 0);
}

TEST_CASE("control: the exceptions capability, the two commands and the opt-in exception event") {
    QuietLogs quiet;
    Rig rig;
    ControlContext context;
    context.version = "0.1.0";
    context.service = &rig.service;
    context.exceptions = &rig.exceptions;
    std::set<std::string> subscribed;
    context.subscribe = [&](const std::set<std::string>& names) { subscribed = names; };
    const std::vector<std::string> caps = broker_capabilities(context);
    CHECK(std::find(caps.begin(), caps.end(), "exceptions") != caps.end());
    ControlContext without;
    without.service = &rig.service;
    const std::vector<std::string> plain = broker_capabilities(without);
    CHECK(std::find(plain.begin(), plain.end(), "exceptions") == plain.end());

    const auto call = [&](const std::string& line) {
        Result<Value> parsed = ipc::json::parse(handle_control_line(line, context));
        REQUIRE(parsed.has_value());
        return *parsed;
    };
    // `exception` is not in the default subscription: it makes the agents hold threads.
    REQUIRE(call(R"({"id":1,"cmd":"subscribe"})").get_bool("ok"));
    CHECK(subscribed.count("exception") == 0);
    CHECK(subscribed.count("diag") == 1);
    REQUIRE(call(R"({"id":2,"cmd":"subscribe","args":{"events":["exception"]}})").get_bool("ok"));
    CHECK(subscribed == std::set<std::string>{"exception"});

    rig.listen(std::make_shared<Collect>());
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(5));
    const Value listed = call(R"({"id":3,"cmd":"exceptions"})");
    REQUIRE(listed.get_bool("ok"));
    CHECK(listed.find("result")->find("pending")->items().size() == 1);
    const Value acted = call(R"({"id":4,"cmd":"exception-action","args":{"pid":4242,"exception_id":5,"action":"continue"}})");
    REQUIRE_MESSAGE(acted.get_bool("ok"), ipc::json::serialize(acted));
    CHECK(acted.find("result")->get_string("action") == "continue");
    const Value missing = call(R"({"id":5,"cmd":"exception-action","args":{"pid":4242,"exception_id":5,"action":"continue"}})");
    CHECK_FALSE(missing.get_bool("ok"));
    CHECK(missing.find("diag")->get_string("id") == "CML5751");

    // A broker without the service answers CML5402 so a client can disable the feature.
    ControlContext bare;
    bare.service = &rig.service;
    Result<Value> unknown = ipc::json::parse(handle_control_line(R"({"id":6,"cmd":"exceptions"})", bare));
    REQUIRE(unknown.has_value());
    CHECK(unknown->find("diag")->get_string("id") == "CML5402");
}

TEST_CASE("exceptions: a second subscriber, or an unchanged state, sends the agents nothing new") {
    QuietLogs quiet;
    Rig rig;
    rig.listen(std::make_shared<Collect>());
    REQUIRE(rig.configs().size() == 1);
    rig.listen(std::make_shared<Collect>());  // another client names `exception`: still on
    rig.exceptions.refresh(true);
    CHECK(rig.configs().size() == 1);
}

TEST_CASE("exceptions: a fault that arrives after the switch-off is released, never left pending") {
    QuietLogs quiet;
    Rig rig;
    const EventBus::Id subscription = rig.listen(std::make_shared<Collect>());
    rig.stop_listening(subscription);
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(4));
    CHECK(rig.exceptions.pending_count() == 0);
    const auto actions = rig.actions();
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].action == ipc::ExceptionActionKind::Ignore);
}

TEST_CASE("exceptions: concurrent refreshes leave the agent with the configuration of the final state") {
    QuietLogs quiet;
    Rig rig;
    auto collector = std::make_shared<Collect>();
    std::thread toggler([&] {
        for (int i = 0; i < 100; ++i) {
            const EventBus::Id id = rig.bus.subscribe(collector, {"exception"});
            rig.exceptions.refresh(true);
            rig.bus.unsubscribe(id);
            rig.exceptions.refresh(true);
        }
    });
    std::thread bystander([&] {
        for (int i = 0; i < 300; ++i) {
            rig.exceptions.refresh(true);
        }
    });
    toggler.join();
    bystander.join();
    rig.exceptions.refresh(true);
    const auto configs = rig.configs();
    REQUIRE_FALSE(configs.empty());
    // The last thing the agent was told is the final state: off. And it was never told the same thing twice in a row.
    CHECK((configs.back().flags & ipc::kExceptionConfigEnabled) == 0);
    for (std::size_t i = 1; i < configs.size(); ++i) {
        CHECK(configs[i].flags != configs[i - 1].flags);
    }
    CHECK_FALSE(rig.exceptions.active());
    CHECK(rig.exceptions.pending_count() == 0);
}

TEST_CASE("exceptions: the same exception_id in two processes needs a pid") {
    QuietLogs quiet;
    Rig rig;
    rig.listen(std::make_shared<Collect>());
    auto other = std::make_shared<AgentLink>(6, [&](const ipc::Message&) -> Result<void> { return Result<void>(); });
    rig.exceptions.agent_ready(other);
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(1));
    rig.exceptions.on_exception(other, 5555, Rig::fault(1));
    Value args = Value::object();
    args.set("exception_id", Value::integer(1));
    args.set("action", Value::string("continue"));
    Result<Value> ambiguous = rig.exceptions.action(args);
    REQUIRE_FALSE(ambiguous.has_value());
    CHECK(ambiguous.error().id() == "CML5755");
    CHECK(rig.exceptions.pending_count() == 2);
    args.set("pid", Value::integer(4242));
    CHECK(rig.exceptions.action(args).has_value());
}

TEST_CASE("control: an empty events array is the default set, not every event (exception stays opt-in)") {
    QuietLogs quiet;
    Rig rig;
    ControlContext context;
    context.service = &rig.service;
    context.exceptions = &rig.exceptions;
    std::set<std::string> subscribed{"sentinel"};
    context.subscribe = [&](const std::set<std::string>& names) { subscribed = names; };
    Result<Value> parsed = ipc::json::parse(handle_control_line(R"({"id":1,"cmd":"subscribe","args":{"events":[]}})", context));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->get_bool("ok"));
    CHECK_FALSE(subscribed.empty());
    CHECK(subscribed.count("exception") == 0);
    CHECK(subscribed.count("progress") == 0);
    CHECK(subscribed.count("diag") == 1);
}
