// Wire-format tests: round trips, malformed frames and limits. Portable (no Win32).
#include "cml/ipc/messages.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include "cml/proc/diag_ids.h"
#include "doctest.h"

using namespace cml;
using namespace cml::ipc;

namespace {

Message round_trip(const Message& in) {
    Result<std::vector<std::uint8_t>> frame = encode_frame(in);
    REQUIRE(frame.has_value());
    CHECK(frame->size() >= kFrameHeaderSize);
    Result<Message> out = decode_frame(frame->data(), frame->size());
    REQUIRE_MESSAGE(out.has_value(), (out.has_value() ? "" : out.error().format()));
    CHECK(type_of(*out) == type_of(in));
    return *out;
}

std::vector<std::uint8_t> frame_bytes(const Message& m) {
    Result<std::vector<std::uint8_t>> frame = encode_frame(m);
    REQUIRE(frame.has_value());
    return *frame;
}

std::uint32_t error_code(const Result<Message>& r) {
    REQUIRE_FALSE(r.has_value());
    return r.error().code;
}

// Sets the 4-byte little-endian payload size of an encoded frame.
void set_payload_size(std::vector<std::uint8_t>& frame, std::uint32_t size) {
    for (int i = 0; i < 4; ++i) {
        frame[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size >> (8 * i));
    }
}

}  // namespace

TEST_CASE("message type ids are stable") {
    CHECK(static_cast<int>(MessageType::Hello) == 0x0001);
    CHECK(static_cast<int>(MessageType::Welcome) == 0x0002);
    CHECK(static_cast<int>(MessageType::Reply) == 0x0003);
    CHECK(static_cast<int>(MessageType::ModuleList) == 0x0010);
    CHECK(static_cast<int>(MessageType::EnableModule) == 0x0011);
    CHECK(static_cast<int>(MessageType::DisableModule) == 0x0012);
    CHECK(static_cast<int>(MessageType::ModuleLoaded) == 0x0013);
    CHECK(static_cast<int>(MessageType::ModuleUnloaded) == 0x0014);
    CHECK(static_cast<int>(MessageType::ReserveRange) == 0x0020);
    CHECK(static_cast<int>(MessageType::ReleaseRange) == 0x0021);
    CHECK(static_cast<int>(MessageType::LoadPatch) == 0x0030);
    CHECK(static_cast<int>(MessageType::PatchLoaded) == 0x0031);
    CHECK(static_cast<int>(MessageType::ApplyFixups) == 0x0032);
    CHECK(static_cast<int>(MessageType::RunInitializers) == 0x0033);
    CHECK(static_cast<int>(MessageType::InstallHooks) == 0x0040);
    CHECK(static_cast<int>(MessageType::HooksInstalled) == 0x0041);
    CHECK(static_cast<int>(MessageType::SyncPointReady) == 0x0042);
    CHECK(static_cast<int>(MessageType::Exception) == 0x0050);
    CHECK(static_cast<int>(MessageType::ExceptionAction) == 0x0051);
    CHECK(static_cast<int>(MessageType::Log) == 0x0060);
    CHECK(static_cast<int>(MessageType::Restart) == 0x0061);
    CHECK(std::string(to_string(MessageType::EnableModule)) == "EnableModule");
    CHECK(std::string(to_string(static_cast<MessageType>(0x7777))) == "Unknown");
}

TEST_CASE("frame layout is little-endian len, type, version, payload") {
    Reply reply;
    reply.request_id = 0x0102030405060708ull;
    reply.status = 5212;
    reply.message = "hi";
    const std::vector<std::uint8_t> frame = frame_bytes(reply);
    // payload = 8 (id) + 4 (status) + 4 (len) + 2 (text) = 18 bytes.
    REQUIRE(frame.size() == kFrameHeaderSize + 18);
    const std::uint8_t expected[] = {
        18, 0, 0, 0,           // payload size
        0x03, 0x00,            // type = Reply
        0x01, 0x00,            // version = 1
        8, 7, 6, 5, 4, 3, 2, 1,  // request id
        0x5C, 0x14, 0, 0,      // status 5212
        2, 0, 0, 0, 'h', 'i',  // message
    };
    REQUIRE(sizeof(expected) == frame.size());
    CHECK(std::memcmp(expected, frame.data(), sizeof(expected)) == 0);
}

TEST_CASE("every implemented message round-trips") {
    SUBCASE("Hello") {
        Hello in;
        in.request_id = 1;
        in.agent_abi_version = 2;
        in.pid = 4242;
        in.mode = AgentMode::Synchronized;
        in.agent_path = "C:\\tools\\cml_agent.dll";
        in.command_line = "game.exe --level \"caf\xC3\xA9\"";
        const auto out = std::get<Hello>(round_trip(in));
        CHECK(out.request_id == 1);
        CHECK(out.protocol_version == kProtocolVersion);
        CHECK(out.agent_abi_version == 2);
        CHECK(out.pid == 4242);
        CHECK(out.mode == AgentMode::Synchronized);
        CHECK(out.agent_path == in.agent_path);
        CHECK(out.command_line == in.command_line);
    }
    SUBCASE("Welcome") {
        Welcome in;
        in.request_id = 9;
        in.agent_id = 77;
        in.broker_version = "0.1.0";
        const auto out = std::get<Welcome>(round_trip(in));
        CHECK(out.request_id == 9);
        CHECK(out.agent_id == 77);
        CHECK(out.broker_version == "0.1.0");
    }
    SUBCASE("ModuleList") {
        ModuleList in;
        in.request_id = 2;
        in.modules.push_back({0x140000000ull, 0x20000, "C:\\a\\game.exe"});
        in.modules.push_back({0x7FF800000000ull, 0x1000, ""});
        const auto out = std::get<ModuleList>(round_trip(in));
        REQUIRE(out.modules.size() == 2);
        CHECK(out.modules[0].base == 0x140000000ull);
        CHECK(out.modules[0].size == 0x20000);
        CHECK(out.modules[0].path == "C:\\a\\game.exe");
        CHECK(out.modules[1].path.empty());
    }
    SUBCASE("empty ModuleList") {
        const auto out = std::get<ModuleList>(round_trip(ModuleList{}));
        CHECK(out.modules.empty());
    }
    SUBCASE("EnableModule / DisableModule") {
        EnableModule en;
        en.request_id = 5;
        en.options = kModuleOptionAllImports;
        en.path = "Game.dll";
        const auto e = std::get<EnableModule>(round_trip(en));
        CHECK(e.request_id == 5);
        CHECK(e.options == kModuleOptionAllImports);
        CHECK(e.path == "Game.dll");

        DisableModule dis;
        dis.request_id = 6;
        dis.path = "Game.dll";
        const auto d = std::get<DisableModule>(round_trip(dis));
        CHECK(d.request_id == 6);
        CHECK(d.path == "Game.dll");
    }
    SUBCASE("ModuleLoaded / ModuleUnloaded") {
        const ModuleEntry entry{0x1000, 0x2000, "C:\\x.dll"};
        const auto l = std::get<ModuleLoaded>(round_trip(ModuleLoaded{entry}));
        CHECK(l.module.base == 0x1000);
        CHECK(l.module.path == "C:\\x.dll");
        const auto u = std::get<ModuleUnloaded>(round_trip(ModuleUnloaded{entry}));
        CHECK(u.module.size == 0x2000);
    }
    SUBCASE("Log") {
        Log in;
        in.level = 3;
        in.text = "something happened";
        const auto out = std::get<Log>(round_trip(in));
        CHECK(out.level == 3);
        CHECK(out.text == "something happened");
    }
    SUBCASE("Reply") {
        Reply in;
        in.request_id = 11;
        in.status = 5212;
        in.message = "module not loaded";
        const auto out = std::get<Reply>(round_trip(in));
        CHECK(out.request_id == 11);
        CHECK(out.status == 5212);
        CHECK(out.message == "module not loaded");
    }
}

TEST_CASE("describe names the message and its key fields") {
    EnableModule m;
    m.request_id = 3;
    m.path = "C:\\x\\fixture_basic.exe";
    const std::string text = describe(m);
    CHECK(text.find("EnableModule") == 0);
    CHECK(text.find("request=3") != std::string::npos);
    CHECK(text.find("fixture_basic.exe") != std::string::npos);
}

TEST_CASE("frame header parsing") {
    SUBCASE("too short") {
        const std::uint8_t bytes[4] = {};
        auto r = parse_frame_header(bytes, sizeof(bytes));
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ids::kFrameHeaderInvalid);
    }
    SUBCASE("oversized payload is rejected before anything is read") {
        std::vector<std::uint8_t> frame(kFrameHeaderSize, 0);
        set_payload_size(frame, kMaxPayloadSize + 1);
        auto r = parse_frame_header(frame.data(), frame.size());
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ids::kFrameHeaderInvalid);
        set_payload_size(frame, 0xFFFFFFFFu);
        CHECK_FALSE(parse_frame_header(frame.data(), frame.size()).has_value());
    }
    SUBCASE("the exact limit is accepted") {
        std::vector<std::uint8_t> frame(kFrameHeaderSize, 0);
        set_payload_size(frame, kMaxPayloadSize);
        auto r = parse_frame_header(frame.data(), frame.size());
        REQUIRE(r.has_value());
        CHECK(r->payload_size == kMaxPayloadSize);
    }
}

TEST_CASE("malformed payloads are diagnosed, never read out of range") {
    Reply reply;
    reply.request_id = 1;
    reply.message = "abcdef";
    const std::vector<std::uint8_t> good = frame_bytes(reply);

    SUBCASE("every truncation of a valid frame fails cleanly") {
        for (std::size_t cut = 0; cut < good.size(); ++cut) {
            const Result<Message> r = decode_frame(good.data(), cut);
            CHECK_FALSE(r.has_value());
        }
        // Truncating the payload while keeping a consistent header is a CML5201.
        for (std::size_t payload = 0; payload + kFrameHeaderSize < good.size(); ++payload) {
            std::vector<std::uint8_t> frame(good.begin(), good.begin() + kFrameHeaderSize + payload);
            set_payload_size(frame, static_cast<std::uint32_t>(payload));
            CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        }
    }
    SUBCASE("trailing bytes") {
        std::vector<std::uint8_t> frame = good;
        frame.push_back(0);
        set_payload_size(frame, static_cast<std::uint32_t>(frame.size() - kFrameHeaderSize));
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("header and body size disagree") {
        std::vector<std::uint8_t> frame = good;
        set_payload_size(frame, 3);
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kFrameHeaderInvalid);
    }
    SUBCASE("string length beyond the payload") {
        std::vector<std::uint8_t> frame = good;
        frame[kFrameHeaderSize + 12] = 0xFF;  // low byte of the message length
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("string length above the field limit") {
        std::vector<std::uint8_t> frame = good;
        frame[kFrameHeaderSize + 12] = 0xFF;
        frame[kFrameHeaderSize + 13] = 0xFF;
        frame[kFrameHeaderSize + 14] = 0xFF;
        frame[kFrameHeaderSize + 15] = 0xFF;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("invalid UTF-8 in a string") {
        std::vector<std::uint8_t> frame = good;
        frame[frame.size() - 1] = 0xFF;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("unsupported payload version") {
        std::vector<std::uint8_t> frame = good;
        frame[6] = 2;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) ==
              ids::kMessageVersionUnsupported);
    }
    SUBCASE("unknown and not-yet-implemented types") {
        std::vector<std::uint8_t> frame = good;
        frame[4] = 0xEE;
        frame[5] = 0x7E;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kMessageTypeUnsupported);
        frame[4] = static_cast<std::uint8_t>(MessageType::LoadPatch);
        frame[5] = 0;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kMessageTypeUnsupported);
    }
    SUBCASE("a lying module count cannot force a huge allocation") {
        ModuleList list;
        list.request_id = 1;
        std::vector<std::uint8_t> frame = frame_bytes(list);
        // count field follows the 8-byte request id.
        frame[kFrameHeaderSize + 8] = 0xFF;
        frame[kFrameHeaderSize + 9] = 0xFF;
        frame[kFrameHeaderSize + 10] = 0xFF;
        frame[kFrameHeaderSize + 11] = 0x7F;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        // Within the 16384 limit but not backed by payload bytes.
        frame[kFrameHeaderSize + 8] = 0x00;
        frame[kFrameHeaderSize + 9] = 0x10;
        frame[kFrameHeaderSize + 10] = 0x00;
        frame[kFrameHeaderSize + 11] = 0x00;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("unknown enum values") {
        Hello hello;
        std::vector<std::uint8_t> frame = frame_bytes(hello);
        frame[kFrameHeaderSize + 8 + 4 + 4 + 4] = 9;  // mode
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        Log log;
        frame = frame_bytes(log);
        frame[kFrameHeaderSize] = 77;  // level
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("null payload with a size") {
        CHECK(error_code(decode_message(static_cast<std::uint16_t>(MessageType::Reply),
                                        kMessageVersion, nullptr, 4)) == ids::kPayloadMalformed);
    }
    SUBCASE("empty payload for a type that needs fields") {
        CHECK(error_code(decode_message(static_cast<std::uint16_t>(MessageType::Hello),
                                        kMessageVersion, nullptr, 0)) == ids::kPayloadMalformed);
    }
}

TEST_CASE("pseudo-random garbage never crashes the decoder") {
    std::uint32_t state = 0x12345678u;
    auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    const std::uint16_t types[] = {0x0001, 0x0002, 0x0003, 0x0010, 0x0011,
                                   0x0012, 0x0013, 0x0014, 0x0040, 0x0041, 0x0060};
    for (int i = 0; i < 4000; ++i) {
        std::vector<std::uint8_t> payload(next() % 64);
        for (std::uint8_t& b : payload) {
            b = static_cast<std::uint8_t>(next());
        }
        const std::uint16_t type = types[next() % (sizeof(types) / sizeof(types[0]))];
        const Result<Message> r =
            decode_message(type, kMessageVersion, payload.data(), payload.size());
        if (r.has_value()) {
            // Anything accepted must re-encode.
            CHECK(encode_frame(*r).has_value());
        }
    }
}

TEST_CASE("encoder enforces the string and payload limits") {
    SUBCASE("string at the limit is fine, one over is refused") {
        Log log;
        log.text.assign(kMaxStringBytes, 'a');
        CHECK(encode_frame(log).has_value());
        log.text.push_back('a');
        Result<std::vector<std::uint8_t>> r = encode_frame(log);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ids::kMessageTooLarge);
    }
    SUBCASE("payload over 1 MiB is refused") {
        ModuleList list;
        ModuleEntry entry;
        entry.path.assign(kMaxStringBytes, 'p');
        for (int i = 0; i < 5; ++i) {
            list.modules.push_back(entry);
        }
        Result<std::vector<std::uint8_t>> r = encode_frame(list);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().code == ids::kMessageTooLarge);
    }
}

TEST_CASE("UTF-8 validation") {
    CHECK(is_valid_utf8(""));
    CHECK(is_valid_utf8("plain ascii"));
    CHECK(is_valid_utf8("caf\xC3\xA9"));
    CHECK(is_valid_utf8("\xE2\x82\xAC"));
    CHECK(is_valid_utf8("\xF0\x9F\x98\x80"));
    CHECK_FALSE(is_valid_utf8("\xC0\xAF"));          // overlong
    CHECK_FALSE(is_valid_utf8("\xE0\x80\xAF"));      // overlong
    CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));      // surrogate
    CHECK_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));  // above U+10FFFF
    CHECK_FALSE(is_valid_utf8("\xC3"));              // truncated
    CHECK_FALSE(is_valid_utf8("\x80"));              // lone continuation
    CHECK_FALSE(is_valid_utf8("\xFF"));
}

TEST_CASE("utf8_cut_point never splits a multi-byte character") {
    const std::string text = "ab\xE2\x82\xAC" "cd";  // a, b, euro sign (3 bytes), c, d
    CHECK(ipc::utf8_cut_point(text, 100) == text.size());
    CHECK(ipc::utf8_cut_point(text, 2) == 2);
    CHECK(ipc::utf8_cut_point(text, 3) == 2);  // would cut after the first byte of the euro sign
    CHECK(ipc::utf8_cut_point(text, 4) == 2);
    CHECK(ipc::utf8_cut_point(text, 5) == 5);
    CHECK(ipc::utf8_cut_point("", 0) == 0);
}

TEST_CASE("sanitize_utf8 repairs invalid input and cuts on a boundary") {
    CHECK(ipc::sanitize_utf8("plain", 100) == "plain");
    CHECK(ipc::sanitize_utf8("caf\xC3\xA9", 100) == "caf\xC3\xA9");
    // A lone lead byte and a stray byte become U+FFFD.
    const std::string repaired = ipc::sanitize_utf8("a\xE2z\xFFz", 100);
    CHECK(repaired == "a\xEF\xBF\xBD" "z\xEF\xBF\xBD" "z");
    CHECK(ipc::is_valid_utf8(repaired));
    // Cutting never leaves half a character, and the result always fits.
    const std::string euros = "\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC";
    for (std::size_t limit = 0; limit <= 10; ++limit) {
        const std::string cut = ipc::sanitize_utf8(euros, limit);
        CHECK(cut.size() <= limit);
        CHECK(ipc::is_valid_utf8(cut));
    }
    CHECK(ipc::sanitize_utf8(euros, 7).size() == 6);
}

TEST_CASE("a log line cut at 1023 bytes inside a multi-byte character becomes a valid Log") {
    // What cml::log_write produces for a long message: 1022 ASCII bytes then the first byte of a
    // 3-byte character (the C runtime cuts by bytes).
    std::string cut(1022, 'x');
    cut += '\xE2';
    REQUIRE_FALSE(ipc::is_valid_utf8(cut));
    ipc::Log log;
    log.level = 3;
    log.text = cut;
    // The encoder refuses to put invalid text on the wire...
    Result<std::vector<std::uint8_t>> refused = encode_frame(log);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ids::kPayloadMalformed);
    // ... and the sanitised text goes through.
    log.text = ipc::sanitize_utf8(cut, 2000);
    CHECK(encode_frame(log).has_value());
    CHECK(log.text.size() == 1022 + 3);  // the dangling lead byte became U+FFFD
}
