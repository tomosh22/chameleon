// WP-48 (ADR-0044): ModuleEvents and the agent ABI field of Welcome on the wire. Portable (no Win32).
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "cml/ipc/messages.h"
#include "cml/proc/diag_ids.h"
#include "doctest.h"

using namespace cml;
using namespace cml::ipc;

namespace {

std::vector<std::uint8_t> frame_of(const Message& m) {
    Result<std::vector<std::uint8_t>> frame = encode_frame(m);
    REQUIRE(frame.has_value());
    return *frame;
}

Message decoded(const std::vector<std::uint8_t>& frame) {
    Result<Message> out = decode_frame(frame.data(), frame.size());
    REQUIRE_MESSAGE(out.has_value(), (out.has_value() ? "" : out.error().format()));
    return *out;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

ModuleEvent event(ModuleEventKind kind, std::uint64_t seq, std::uint64_t base, const char* path) {
    ModuleEvent e;
    e.kind = kind;
    e.seq = seq;
    e.module = ModuleEntry{base, 0x10000, path};
    return e;
}

}  // namespace

TEST_CASE("WP-48: ModuleEvents has a stable id, is an event (no request id) and round-trips with its order") {
    CHECK(static_cast<int>(MessageType::ModuleEvents) == 0x0015);
    CHECK(is_implemented(MessageType::ModuleEvents));
    CHECK_FALSE(is_request_type(0x0015));
    CHECK(std::string(to_string(MessageType::ModuleEvents)) == "ModuleEvents");

    ModuleEvents in;
    in.events.push_back(event(ModuleEventKind::Unloaded, 41, 0x180000000ull, "C:\\game\\plugin.dll"));
    in.events.push_back(event(ModuleEventKind::Loaded, 42, 0x180000000ull, "C:\\game\\plugin.dll"));
    in.events.push_back(event(ModuleEventKind::Loaded, 0, 0x190000000ull, "C:\\game\\\xC3\xA9.dll"));
    const std::vector<std::uint8_t> frame = frame_of(in);
    CHECK_FALSE(peek_request_id(0x0015, frame.data() + kFrameHeaderSize, frame.size() - kFrameHeaderSize).has_value());
    const auto out = std::get<ModuleEvents>(decoded(frame));
    CHECK(out.flags == 0);
    CHECK(out.reason == 0);
    CHECK(out.lost == 0);
    REQUIRE(out.events.size() == 3);
    CHECK(out.events[0].kind == ModuleEventKind::Unloaded);
    CHECK(out.events[0].seq == 41);
    CHECK(out.events[1].kind == ModuleEventKind::Loaded);
    CHECK(out.events[1].seq == 42);
    CHECK(out.events[1].module.base == 0x180000000ull);
    CHECK(out.events[1].module.size == 0x10000);
    CHECK(out.events[2].module.path == "C:\\game\\\xC3\xA9.dll");
    CHECK(describe(out).find("ModuleEvents flags=0 reason=0 lost=0 events=3 [unload seq=41") == 0);

    ModuleEvents resync;
    resync.flags = kModuleEventsResync;
    resync.reason = 4721;
    resync.lost = 17;
    const auto back = std::get<ModuleEvents>(decoded(frame_of(resync)));
    CHECK(back.flags == kModuleEventsResync);
    CHECK(back.reason == 4721);
    CHECK(back.lost == 17);
    CHECK(back.events.empty());
}

TEST_CASE("WP-48: a malformed ModuleEvents is refused (unknown flag or kind, a count the payload cannot hold, trailing bytes)") {
    const auto payload = [](std::uint32_t flags, std::uint32_t count, std::uint32_t kind, bool trailing) {
        std::vector<std::uint8_t> p;
        put_u32(p, flags);
        put_u32(p, 0);
        put_u64(p, 0);
        put_u32(p, count);
        if (count != 0 && kind != 0) {
            put_u32(p, kind);
            put_u64(p, 1);
            put_u64(p, 0x1000);
            put_u64(p, 0x100);
            put_u32(p, 0);  // empty path
        }
        if (trailing) {
            p.push_back(0);
        }
        return p;
    };
    const auto decode = [](const std::vector<std::uint8_t>& p) {
        return decode_message(static_cast<std::uint16_t>(MessageType::ModuleEvents), kMessageVersion, p.data(), p.size());
    };
    CHECK(decode(payload(0, 1, 1, false)).has_value());
    CHECK(decode(payload(0, 1, 2, false)).has_value());
    for (const std::vector<std::uint8_t>& bad : {payload(2, 0, 0, false), payload(0, 1, 3, false), payload(0, 1, 0, false),
                                                 payload(0, 1000, 1, false), payload(0, kMaxModuleEvents + 1, 0, false),
                                                 payload(0, 1, 1, true)}) {
        const Result<Message> r = decode(bad);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ids::kPayloadMalformed);
    }
}

TEST_CASE("WP-48: Welcome carries the agent ABI only when it is set; without it the frame is the one an agent before WP-48 decodes") {
    Welcome plain;
    plain.request_id = 7;
    plain.agent_id = 3;
    plain.broker_version = "0.1.0";
    const std::vector<std::uint8_t> old_layout = frame_of(plain);
    // request id + protocol + agent id + broker version (u32 length + 5 bytes): nothing after it, so an old agent's decoder (which
    // rejects trailing bytes) still takes it.
    CHECK(old_layout.size() == kFrameHeaderSize + 8 + 4 + 8 + 4 + 5);
    CHECK(std::get<Welcome>(decoded(old_layout)).agent_abi_version == 0);

    Welcome with_abi = plain;
    with_abi.agent_abi_version = kAgentAbiModuleEvents;
    const std::vector<std::uint8_t> new_layout = frame_of(with_abi);
    CHECK(new_layout.size() == old_layout.size() + 4);
    const auto back = std::get<Welcome>(decoded(new_layout));
    CHECK(back.agent_abi_version == kAgentAbiModuleEvents);
    CHECK(back.broker_version == "0.1.0");
    CHECK(describe(back).find(" abi=3") != std::string::npos);

    // A cut trailing field is malformed, not read past the end.
    std::vector<std::uint8_t> cut(new_layout.begin(), new_layout.end() - 2);
    cut[0] = static_cast<std::uint8_t>(cut.size() - kFrameHeaderSize);
    const Result<Message> r = decode_frame(cut.data(), cut.size());
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ids::kPayloadMalformed);
}

TEST_CASE("WP-48: the agent ABI numbers") {
    CHECK(kAgentAbiSnapshots == 2);
    CHECK(kAgentAbiModuleEvents == kAgentAbiSnapshots + 1);  // ADR-0044: bumped by one
    CHECK(kAgentAbiVersion == kAgentAbiModuleEvents);
}
