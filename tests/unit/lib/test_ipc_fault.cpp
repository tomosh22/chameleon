// Wire format of ArmFault (0x00C0) and the failure-injection stage table, WP-28. Portable (no Win32).
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "cml/ipc/fault_stages.h"
#include "cml/ipc/messages.h"
#include "doctest.h"

using namespace cml;
using namespace cml::ipc;

TEST_CASE("ArmFault: type id 0x00C0, implemented, a request with its id first") {
    CHECK(static_cast<std::uint16_t>(MessageType::ArmFault) == 0x00C0);
    CHECK(std::string(to_string(MessageType::ArmFault)) == "ArmFault");
    CHECK(is_implemented(MessageType::ArmFault));
    CHECK(is_request_type(0x00C0));
    ArmFault m;
    m.request_id = 0x1122334455667788ull;
    m.stage = "hook-install";
    m.count = 3;
    Result<std::vector<std::uint8_t>> frame = encode_frame(Message(m));
    REQUIRE_MESSAGE(frame.has_value(), frame.error().format());
    CHECK(peek_request_id(0x00C0, frame->data() + kFrameHeaderSize, frame->size() - kFrameHeaderSize) == std::optional<std::uint64_t>(0x1122334455667788ull));
}

TEST_CASE("ArmFault: round trip, description, trailing bytes and long names are rejected") {
    ArmFault m;
    m.request_id = 9;
    m.stage = "patch-load";
    m.count = 2;
    Result<std::vector<std::uint8_t>> frame = encode_frame(Message(m));
    REQUIRE(frame.has_value());
    Result<Message> back = decode_frame(frame->data(), frame->size());
    REQUIRE_MESSAGE(back.has_value(), back.error().format());
    const auto* arm = std::get_if<ArmFault>(&*back);
    REQUIRE(arm != nullptr);
    CHECK(arm->request_id == 9);
    CHECK(arm->stage == "patch-load");
    CHECK(arm->count == 2);
    CHECK(type_of(*back) == MessageType::ArmFault);
    CHECK(describe(*back) == "ArmFault request=9 stage=\"patch-load\" count=2");

    // Trailing byte.
    std::vector<std::uint8_t> longer = *frame;
    longer.push_back(0);
    longer[0] = static_cast<std::uint8_t>(longer[0] + 1);
    CHECK_FALSE(decode_frame(longer.data(), longer.size()).has_value());
    // Truncated payload.
    std::vector<std::uint8_t> shorter(frame->begin(), frame->end() - 1);
    shorter[0] = static_cast<std::uint8_t>(shorter[0] - 1);
    CHECK_FALSE(decode_frame(shorter.data(), shorter.size()).has_value());
    // A stage name over the limit is malformed.
    ArmFault long_name;
    long_name.stage = std::string(kMaxFaultStageBytes + 1, 'x');
    Result<std::vector<std::uint8_t>> long_frame = encode_frame(Message(long_name));
    REQUIRE(long_frame.has_value());
    Result<Message> rejected = decode_frame(long_frame->data(), long_frame->size());
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().id() == "CML5201");
    // Invalid UTF-8 in the stage name.
    ArmFault bad_utf8;
    bad_utf8.stage = std::string("\xC0\xAF");
    Result<std::vector<std::uint8_t>> bad_frame = encode_frame(Message(bad_utf8));
    if (bad_frame) {
        CHECK_FALSE(decode_frame(bad_frame->data(), bad_frame->size()).has_value());
    }
}

TEST_CASE("fault stages: a table row per stage, in pipeline order, with unique names and ids inside the WP-28 ranges") {
    namespace f = ipc::fault;
    std::set<std::string> names;
    std::set<std::uint32_t> ids;
    std::size_t index = 0;
    for (const f::StageInfo& s : f::kStages) {
        CAPTURE(s.name);
        CHECK(static_cast<std::size_t>(s.stage) == index++);
        CHECK(names.insert(s.name).second);
        CHECK(ids.insert(s.diag_id).second);
        const bool agent_range = s.diag_id >= 4650 && s.diag_id <= 4699;
        const bool broker_range = s.diag_id >= 5960 && s.diag_id <= 5989;
        CHECK((s.agent_side ? agent_range : broker_range));
        CHECK(std::string(s.where).size() > 10);
        CHECK(f::find_stage(s.name) == &s);
        CHECK(&f::info(s.stage) == &s);
    }
    CHECK(index == f::kStageCount);
    CHECK(f::find_stage("none") == nullptr);
    CHECK(f::find_stage("") == nullptr);
    CHECK(f::find_stage("Link") == nullptr);
    // The agent has exactly the four stages of the brief.
    std::vector<std::string> agent;
    for (const f::StageInfo& s : f::kStages) {
        if (s.agent_side) {
            agent.push_back(s.name);
        }
    }
    CHECK(agent == std::vector<std::string>{"reservation", "patch-load", "hook-install", "post-patch-hooks"});
    // The other ids of the ranges are distinct from the stages'.
    for (const std::uint32_t other : {f::ids::kAgentBadArm, f::ids::kAgentNotCompiledIn, f::ids::kBadRequest, f::ids::kNoSuchProcess, f::ids::kArmRefused}) {
        CHECK(ids.insert(other).second);
    }
}
