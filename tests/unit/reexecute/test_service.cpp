// ExceptionService (WP-23): what is offered, `exception_updated`, and the action `reexecute`, against a scripted agent. The evaluation
// itself (PDB records, hooks) is replaced by a test double here and covered by test_recipe.cpp and test_fixture_pdb.cpp. Portable.
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <variant>
#include <vector>

#include "cml/core/exception_service.h"
#include "cml/core/reexecute_diag_ids.h"
#include "cml/ipc/control.h"
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
    std::vector<Value> bodies(const std::string& kind) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Value> out;
        for (const Event& e : events) {
            if (e.body.get_string("event") == kind) {
                out.push_back(e.body);
            }
        }
        return out;
    }
    std::mutex mutex;
    std::vector<Event> events;
};

std::vector<std::string> names_of(const Value& array) {
    std::vector<std::string> out;
    if (array.is_array()) {
        for (const Value& v : array.items()) {
            out.push_back(v.as_string());
        }
    }
    return out;
}

bool contains(const std::vector<std::string>& v, const char* s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

reexecute::Offer refused(std::uint32_t id, const std::string& text) {
    reexecute::Offer o;
    o.diag = id;
    o.reason = "CML" + std::to_string(id) + ": " + text;
    return o;
}

reexecute::Offer offered_step() {
    reexecute::Offer o;
    o.offered = true;
    o.frame_index = 1;
    o.entry_rsp = 0x1100;
    o.target = 0x7FF6DEAD0000ull;
    o.function = "step";
    o.params.push_back(ipc::ReexecuteParam{0, 0, 8, 335, 0x30});
    o.params.push_back(ipc::ReexecuteParam{2, 1, 8, 335, 0x40});
    return o;
}

struct Rig {
    Rig() : service(PipelineEnv{}, &bus, ""), exceptions(&service, &bus) {
        exceptions.set_evaluator_for_testing([this](const std::vector<std::shared_ptr<ModuleState>>&, const ipc::Exception&, const reexecute::HookMap&) {
            std::lock_guard<std::mutex> lock(mutex);
            return verdict;
        });
        link = std::make_shared<AgentLink>(5, [this](const ipc::Message& message) -> Result<void> {
            {
                std::lock_guard<std::mutex> lock(mutex);
                sent.push_back(message);
            }
            const std::optional<std::uint64_t> id = request_id_of(message);
            if (id && (std::holds_alternative<ipc::ExceptionConfig>(message) || std::holds_alternative<ipc::ExceptionAction>(message) ||
                       std::holds_alternative<ipc::Reexecute>(message))) {
                const std::uint32_t status = std::holds_alternative<ipc::ExceptionConfig>(message) ? 0u : action_status.load();
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
        collector = std::make_shared<Collect>();
        bus.subscribe(collector, {"exception"});
        exceptions.refresh(true);
    }
    ~Rig() {
        for (std::thread& t : answerers) {
            t.join();
        }
    }

    void set_verdict(const reexecute::Offer& offer) {
        std::lock_guard<std::mutex> lock(mutex);
        verdict = offer;
    }
    std::vector<ipc::Reexecute> reexecutes() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<ipc::Reexecute> out;
        for (const ipc::Message& m : sent) {
            if (const auto* r = std::get_if<ipc::Reexecute>(&m)) {
                out.push_back(*r);
            }
        }
        return out;
    }

    static ipc::Exception fault(std::uint32_t id) {
        ipc::Exception e;
        e.exception_id = id;
        e.thread_id = 1234;
        e.code = 0xC0000005u;
        e.access = 0;
        e.address = 0x00007FF600001040ull;
        e.module_base = 0x00007FF600000000ull;
        e.module_path = "C:\\build\\fixture_crash_reexecute.exe";
        e.frames.push_back(ipc::ExceptionFrame{0x00007FF600001040ull, 0x1000, 0xFD8, 0, 0, 0x00007FF600001000ull});
        return e;
    }
    static Value args(std::int64_t id, const char* action) {
        Value a = Value::object();
        a.set("pid", Value::integer(4242));
        a.set("exception_id", Value::integer(id));
        a.set("action", Value::string(action));
        return a;
    }
    void patch_applied() { bus.publish(make_patch_applied_event(4242, "fixture_crash_reexecute.exe", 1, 1, 0, 0)); }
    void patch_reverted() { bus.publish(make_patch_reverted_event(4242, "fixture_crash_reexecute.exe", 0)); }

    EventBus bus;
    ReloadService service;
    ExceptionService exceptions;
    std::shared_ptr<AgentLink> link;
    std::shared_ptr<Collect> collector;
    std::mutex mutex;
    reexecute::Offer verdict = refused(reexecute::diag::kNoReplacement, "no patch applied since the fault has replaced a function on this thread's stack");
    std::vector<ipc::Message> sent;
    std::vector<std::thread> answerers;
    std::atomic<std::uint32_t> action_status{0};
};

}  // namespace

TEST_CASE("reexecute: the exception event lists the actions that are offered and says why reexecute is not") {
    QuietLogs quiet;
    Rig rig;
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(1));
    const std::vector<Value> events = rig.collector->bodies("exception");
    REQUIRE(events.size() == 1);
    const std::vector<std::string> actions = names_of(*events[0].find("actions"));
    CHECK(actions == std::vector<std::string>{"continue", "leave", "ignore"});
    const Value* reasons = events[0].find("reasons");
    REQUIRE(reasons != nullptr);
    CHECK(reasons->get_string("reexecute").find("CML5900") == 0);
    // A thread that is listed again by `exceptions` carries the same.
    Result<Value> pending = rig.exceptions.pending(Value::object());
    REQUIRE(pending.has_value());
    CHECK(pending->find("pending")->items()[0].find("reasons")->get_string("reexecute").find("CML5900") == 0);
}

TEST_CASE("reexecute: without frames from the agent the real evaluation says so (CML5907)") {
    QuietLogs quiet;
    EventBus bus;
    ReloadService service(PipelineEnv{}, &bus, "");
    ExceptionService exceptions(&service, &bus);
    auto link = std::make_shared<AgentLink>(9, [](const ipc::Message&) -> Result<void> { return Result<void>(); });
    exceptions.agent_ready(link);
    auto collector = std::make_shared<Collect>();
    bus.subscribe(collector, {"exception"});
    exceptions.refresh(false);
    ipc::Exception e = Rig::fault(1);
    e.frames.clear();
    exceptions.on_exception(link, 4242, e);
    const std::vector<Value> events = collector->bodies("exception");
    REQUIRE(events.size() == 1);
    CHECK(events[0].find("reasons")->get_string("reexecute").find("CML5907") == 0);
    CHECK_FALSE(contains(names_of(*events[0].find("actions")), "reexecute"));
}

TEST_CASE("reexecute: a patch that replaces the function turns the action on - exception_updated - and a revert turns it off") {
    QuietLogs quiet;
    Rig rig;
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(7));

    rig.patch_applied();  // nothing changed: no event
    CHECK(rig.collector->bodies("exception_updated").empty());

    rig.set_verdict(offered_step());
    rig.patch_applied();
    const std::vector<Value> updates = rig.collector->bodies("exception_updated");
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].get_integer("pid") == 4242);
    CHECK(updates[0].get_integer("exception_id") == 7);
    CHECK(contains(names_of(*updates[0].find("actions")), "reexecute"));
    CHECK(contains(names_of(*updates[0].find("actions")), "continue"));
    CHECK(updates[0].find("reasons")->find("reexecute") == nullptr);
    CHECK(updates[0].get_string("reexecute_function") == "step");
    // The pending list reflects it.
    Result<Value> pending = rig.exceptions.pending(Value::object());
    REQUIRE(pending.has_value());
    CHECK(contains(names_of(*pending->find("pending")->items()[0].find("actions")), "reexecute"));

    rig.patch_applied();  // still the same verdict: no second event
    CHECK(rig.collector->bodies("exception_updated").size() == 1);

    rig.set_verdict(refused(reexecute::diag::kNoReplacement, "the revert put the old code back"));
    rig.patch_reverted();
    const std::vector<Value> after_revert = rig.collector->bodies("exception_updated");
    REQUIRE(after_revert.size() == 2);
    CHECK_FALSE(contains(names_of(*after_revert[1].find("actions")), "reexecute"));
    CHECK(after_revert[1].find("reasons")->get_string("reexecute").find("revert") != std::string::npos);
}

TEST_CASE("reexecute: a changed reason is news too (a patch replaced an optimised function and re-execute is still not offered)") {
    QuietLogs quiet;
    Rig rig;
    rig.set_verdict(refused(reexecute::diag::kOptimised, "opt_step is in a compiland built with optimisation"));
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(2));
    rig.set_verdict(refused(reexecute::diag::kOptimised, "opt_step is in a compiland built with optimisation; a patch has replaced opt_step, but it cannot be re-executed"));
    rig.patch_applied();
    const std::vector<Value> updates = rig.collector->bodies("exception_updated");
    REQUIRE(updates.size() == 1);
    CHECK_FALSE(contains(names_of(*updates[0].find("actions")), "reexecute"));
    CHECK(updates[0].find("reasons")->get_string("reexecute").find("CML5901") == 0);
}

TEST_CASE("reexecute: events of another process do not touch the thread") {
    QuietLogs quiet;
    Rig rig;
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(3));
    rig.set_verdict(offered_step());
    rig.bus.publish(make_patch_applied_event(999, "other.exe", 1, 1, 0, 0));
    CHECK(rig.collector->bodies("exception_updated").empty());
}

TEST_CASE("reexecute: the action sends the recipe to the agent and releases the thread") {
    QuietLogs quiet;
    Rig rig;
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(7));
    rig.set_verdict(offered_step());
    rig.patch_applied();

    Result<Value> result = rig.exceptions.action(Rig::args(7, "reexecute"));
    REQUIRE_MESSAGE(result.has_value(), result.error().format());
    CHECK(result->get_string("action") == "reexecute");
    CHECK(result->get_integer("exception_id") == 7);
    const std::vector<ipc::Reexecute> sent = rig.reexecutes();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].exception_id == 7);
    CHECK(sent[0].frame_index == 1);
    CHECK(sent[0].entry_rsp == 0x1100);
    CHECK(sent[0].target == 0x7FF6DEAD0000ull);
    REQUIRE(sent[0].params.size() == 2);
    CHECK(sent[0].params[1].is_float == 1);
    CHECK(sent[0].params[1].offset == 0x40);
    CHECK(rig.exceptions.pending_count() == 0);
}

TEST_CASE("reexecute: asking for it while it is not offered is refused with CML5908 and the thread stays held") {
    QuietLogs quiet;
    Rig rig;
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(7));
    Result<Value> result = rig.exceptions.action(Rig::args(7, "reexecute"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == reexecute::diag::kNotOffered);
    CHECK(result.error().message.find("CML5900") != std::string::npos);  // the reason travels with the refusal
    CHECK(rig.reexecutes().empty());
    CHECK(rig.exceptions.pending_count() == 1);
    // The other actions still work.
    REQUIRE(rig.exceptions.action(Rig::args(7, "leave")).has_value());
}

TEST_CASE("reexecute: an agent that cannot apply the recipe leaves the thread held for another action") {
    QuietLogs quiet;
    Rig rig;
    rig.exceptions.on_exception(rig.link, 4242, Rig::fault(7));
    rig.set_verdict(offered_step());
    rig.patch_applied();
    rig.action_status = 4601;  // CML4601: the stack changed
    Result<Value> result = rig.exceptions.action(Rig::args(7, "reexecute"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == 4601);
    CHECK(rig.exceptions.pending_count() == 1);
    rig.action_status = 0;
    REQUIRE(rig.exceptions.action(Rig::args(7, "leave")).has_value());
}

TEST_CASE("reexecute: the capability is advertised with the exception commands") {
    CHECK(std::string(ipc::capability::kReexecute) == "reexecute");
}
