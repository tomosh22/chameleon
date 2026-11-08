// Agent-side failure injection state and the ArmFault handler (WP-28). Portable; the stages' effects (reservation, patch load,
// the installer's window, the rollback after the post-patch-hooks stage) are exercised by `unit.agent_hook` and by the
// scenario `inject_stages` against the real agent.
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "fault/fault_injection.h"

namespace f = cml::ipc::fault;
namespace af = cml::agent::fault;

namespace {

struct Disarmed {
    Disarmed() { af::disarm_all(); }
    ~Disarmed() { af::disarm_all(); }
};

cml::ipc::Reply reply_of(const cml::ipc::Message& m) {
    const auto* r = std::get_if<cml::ipc::Reply>(&m);
    REQUIRE(r != nullptr);
    return *r;
}

cml::ipc::Message arm_message(const std::string& stage, std::uint32_t count, std::uint64_t id = 7) {
    cml::ipc::ArmFault m;
    m.request_id = id;
    m.stage = stage;
    m.count = count;
    return m;
}

}  // namespace

TEST_CASE("agent fault: ArmFault 'none' succeeds in every build, and is recognised") {
    Disarmed guard;
    CHECK(af::is_arm_request(arm_message("link", 1)));
    CHECK_FALSE(af::is_arm_request(cml::ipc::Message(cml::ipc::Reply{})));
    // An agent without injection has nothing armed: disarming is a success there too.
    const cml::ipc::Reply none = reply_of(af::handle_arm_request(arm_message("none", 1, 9)));
    CHECK(none.status == 0);
    CHECK(none.request_id == 9);
}

#if !defined(CML_FAULT_INJECTION)

TEST_CASE("agent fault: without CML_FAULT_INJECTION nothing fires and arming is CML4655") {
    CHECK_FALSE(af::consume(f::Stage::HookInstall));
    const cml::ipc::Reply r = reply_of(af::handle_arm_request(arm_message("patch-load", 1)));
    CHECK(r.status == f::ids::kAgentNotCompiledIn);
    CHECK(r.request_id == 7);
}

#else

TEST_CASE("agent fault: consume uses up exactly the armed passes") {
    Disarmed guard;
    CHECK_FALSE(af::consume(f::Stage::HookInstall));
    af::arm(f::Stage::HookInstall, 2);
    CHECK(af::armed(f::Stage::HookInstall) == 2);
    CHECK_FALSE(af::consume(f::Stage::PatchLoad));  // another stage is not affected
    CHECK(af::consume(f::Stage::HookInstall));
    CHECK(af::consume(f::Stage::HookInstall));
    CHECK_FALSE(af::consume(f::Stage::HookInstall));
    CHECK(af::armed(f::Stage::HookInstall) == 0);
    af::arm(f::Stage::Reservation, 5);
    af::arm(f::Stage::Reservation, 1);  // re-arming replaces
    CHECK(af::armed(f::Stage::Reservation) == 1);
    af::disarm_all();
    CHECK(af::armed(f::Stage::Reservation) == 0);
}

TEST_CASE("agent fault: concurrent consumers use each pass exactly once") {
    Disarmed guard;
    constexpr std::uint32_t kPasses = 1000;
    af::arm(f::Stage::PostPatchHooks, kPasses);
    std::atomic<std::uint32_t> taken{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i) {
                if (af::consume(f::Stage::PostPatchHooks)) {
                    ++taken;
                }
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    CHECK(taken.load() == kPasses);
    CHECK(af::armed(f::Stage::PostPatchHooks) == 0);
}

TEST_CASE("agent fault: ArmFault arms an agent stage, count 0 disarms that stage only, 'none' disarms everything") {
    Disarmed guard;
    const cml::ipc::Reply ok = reply_of(af::handle_arm_request(arm_message("patch-load", 3, 42)));
    CHECK(ok.status == 0);
    CHECK(ok.request_id == 42);
    CHECK(af::armed(f::Stage::PatchLoad) == 3);
    CHECK(reply_of(af::handle_arm_request(arm_message("hook-install", 1))).status == 0);
    CHECK(reply_of(af::handle_arm_request(arm_message("patch-load", 0))).status == 0);
    CHECK(af::armed(f::Stage::PatchLoad) == 0);
    CHECK(af::armed(f::Stage::HookInstall) == 1);
    CHECK(reply_of(af::handle_arm_request(arm_message("none", 1))).status == 0);
    CHECK(af::armed(f::Stage::HookInstall) == 0);
}

TEST_CASE("agent fault: unknown stages, stages of the broker and bad counts are CML4654 and arm nothing") {
    Disarmed guard;
    for (const char* stage : {"", "nope", "link", "compile", "Patch-Load", "none "}) {
        CAPTURE(stage);
        const cml::ipc::Reply r = reply_of(af::handle_arm_request(arm_message(stage, 1)));
        CHECK(r.status == f::ids::kAgentBadArm);
        CHECK_FALSE(r.message.empty());
    }
    CHECK(reply_of(af::handle_arm_request(arm_message("patch-load", f::kMaxCount + 1))).status == f::ids::kAgentBadArm);
    for (const f::StageInfo& s : f::kStages) {
        CHECK(af::armed(s.stage) == 0);
    }
}

#endif
