// Wire format of InstallHooks (0x0040) and HooksInstalled (0x0041), WP-10. Portable (no Win32).
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

void set_payload_size(std::vector<std::uint8_t>& frame, std::uint32_t size) {
    for (int i = 0; i < 4; ++i) {
        frame[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size >> (8 * i));
    }
}

InstallHooks sample_install_hooks() {
    InstallHooks in;
    in.request_id = 0x1122334455667788ull;
    in.flags = 0;
    in.hooks.push_back({0x00007FF612340000ull, 0x00007FF612350040ull, "compute_value"});
    in.hooks.push_back({0x0000000140001000ull, 0x0000000140002000ull, ""});
    in.hooks.push_back({0x0000000140003000ull, 0x0000000140004000ull, "caf\xC3\xA9::f(int)"});
    return in;
}

HooksInstalled sample_hooks_installed() {
    HooksInstalled in;
    in.request_id = 77;
    in.status = 4305;
    in.message = "thread 12 kept executing inside its first bytes";
    in.attempts = 3;
    in.retries = 50;
    in.threads = 17;
    in.window_ms = 4;
    in.results.push_back({HookStrategy::PaddingTwoByte, HookResultStatus::Installed, 0, ""});
    in.results.push_back({HookStrategy::FiveByte, HookResultStatus::Failed, 4305, "details"});
    in.results.push_back({HookStrategy::None, HookResultStatus::NotAttempted, 4313, ""});
    in.results.push_back({HookStrategy::FiveByte, HookResultStatus::Unchanged, 0, ""});
    return in;
}

}  // namespace

TEST_CASE("InstallHooks and HooksInstalled round-trip") {
    SUBCASE("InstallHooks") {
        const auto in = sample_install_hooks();
        const auto out = std::get<InstallHooks>(round_trip(in));
        CHECK(out.request_id == in.request_id);
        CHECK(out.flags == 0);
        REQUIRE(out.hooks.size() == 3);
        CHECK(out.hooks[0].old_entry == 0x00007FF612340000ull);
        CHECK(out.hooks[0].new_entry == 0x00007FF612350040ull);
        CHECK(out.hooks[0].name == "compute_value");
        CHECK(out.hooks[1].name.empty());
        CHECK(out.hooks[2].name == in.hooks[2].name);
    }
    SUBCASE("InstallHooks with the revert flag, and an empty list") {
        InstallHooks in = sample_install_hooks();
        in.flags = kInstallHooksRevert;
        CHECK(std::get<InstallHooks>(round_trip(in)).flags == kInstallHooksRevert);
        CHECK(std::get<InstallHooks>(round_trip(InstallHooks{})).hooks.empty());
    }
    SUBCASE("HooksInstalled") {
        const auto in = sample_hooks_installed();
        const auto out = std::get<HooksInstalled>(round_trip(in));
        CHECK(out.request_id == 77);
        CHECK(out.status == 4305);
        CHECK(out.message == in.message);
        CHECK(out.attempts == 3);
        CHECK(out.retries == 50);
        CHECK(out.threads == 17);
        CHECK(out.window_ms == 4);
        REQUIRE(out.results.size() == 4);
        CHECK(out.results[0].strategy == HookStrategy::PaddingTwoByte);
        CHECK(out.results[0].status == HookResultStatus::Installed);
        CHECK(out.results[1].status == HookResultStatus::Failed);
        CHECK(out.results[1].diag == 4305);
        CHECK(out.results[1].message == "details");
        CHECK(out.results[2].status == HookResultStatus::NotAttempted);
        CHECK(out.results[2].diag == 4313);
        CHECK(out.results[3].status == HookResultStatus::Unchanged);
    }
    SUBCASE("the largest allowed request fits in one frame") {
        InstallHooks in;
        in.request_id = 1;
        for (std::uint32_t i = 0; i < kMaxHooksPerRequest; ++i) {
            in.hooks.push_back({0x140000000ull + i * 16, 0x150000000ull + i * 16, "fn" + std::to_string(i)});
        }
        CHECK(std::get<InstallHooks>(round_trip(in)).hooks.size() == kMaxHooksPerRequest);
        HooksInstalled out;
        out.request_id = 1;
        out.results.assign(kMaxHooksPerRequest, HookResult{});
        CHECK(std::get<HooksInstalled>(round_trip(out)).results.size() == kMaxHooksPerRequest);
    }
}

TEST_CASE("InstallHooks wire layout is fixed") {
    InstallHooks in;
    in.request_id = 0x0102030405060708ull;
    in.flags = kInstallHooksRevert;
    in.hooks.push_back({0x1111111111111111ull, 0x2222222222222222ull, "ab"});
    const std::vector<std::uint8_t> frame = frame_bytes(in);
    // payload = id 8 + flags 4 + count 4 + (8 + 8 + 4 + 2) = 38
    const std::uint8_t expected[] = {
        38, 0, 0, 0,             // payload size
        0x40, 0x00,              // type = InstallHooks
        0x01, 0x00,              // version 1
        8, 7, 6, 5, 4, 3, 2, 1,  // request id
        1, 0, 0, 0,              // flags: revert
        1, 0, 0, 0,              // count
        0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
        0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
        2, 0, 0, 0, 'a', 'b',
    };
    REQUIRE(sizeof(expected) == frame.size());
    CHECK(std::memcmp(expected, frame.data(), sizeof(expected)) == 0);
}

TEST_CASE("malformed InstallHooks / HooksInstalled are diagnosed, never read out of range") {
    const std::vector<std::uint8_t> good_request = frame_bytes(sample_install_hooks());
    const std::vector<std::uint8_t> good_reply = frame_bytes(sample_hooks_installed());

    SUBCASE("every truncation of either message is rejected") {
        for (const std::vector<std::uint8_t>* good : {&good_request, &good_reply}) {
            for (std::size_t cut = 0; cut + kFrameHeaderSize < good->size(); ++cut) {
                std::vector<std::uint8_t> frame(
                    good->begin(), good->begin() + static_cast<std::ptrdiff_t>(kFrameHeaderSize + cut));
                set_payload_size(frame, static_cast<std::uint32_t>(cut));
                CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
            }
        }
    }
    SUBCASE("trailing bytes") {
        std::vector<std::uint8_t> frame = good_request;
        frame.push_back(0);
        set_payload_size(frame, static_cast<std::uint32_t>(frame.size() - kFrameHeaderSize));
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("unknown flag bits") {
        std::vector<std::uint8_t> frame = good_request;
        frame[kFrameHeaderSize + 8] = 0x02;  // flags follow the request id
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("a lying hook count cannot force a huge allocation") {
        std::vector<std::uint8_t> frame = good_request;
        frame[kFrameHeaderSize + 12] = 0xFF;  // count follows id + flags
        frame[kFrameHeaderSize + 13] = 0xFF;
        frame[kFrameHeaderSize + 14] = 0xFF;
        frame[kFrameHeaderSize + 15] = 0x7F;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        frame[kFrameHeaderSize + 12] = 0x00;  // 0x7FFFFF00 is still above the limit
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        frame[kFrameHeaderSize + 12] = 0x10;  // 0x10: above what the payload holds
        frame[kFrameHeaderSize + 13] = 0x00;
        frame[kFrameHeaderSize + 14] = 0x00;
        frame[kFrameHeaderSize + 15] = 0x00;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("a lying result count") {
        HooksInstalled in;
        in.request_id = 1;
        std::vector<std::uint8_t> frame = frame_bytes(in);
        // id 8, status 4, message length 4 (empty), 4 u32 stats = 16, then the count.
        const std::size_t count_at = kFrameHeaderSize + 8 + 4 + 4 + 16;
        frame[count_at] = 5;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("unknown strategy and status values") {
        HooksInstalled in;
        in.request_id = 1;
        in.results.push_back({HookStrategy::FiveByte, HookResultStatus::Installed, 0, ""});
        const std::vector<std::uint8_t> frame = frame_bytes(in);
        const std::size_t first = kFrameHeaderSize + 8 + 4 + 4 + 16 + 4;  // first result
        std::vector<std::uint8_t> bad_strategy = frame;
        bad_strategy[first] = 3;
        CHECK(error_code(decode_frame(bad_strategy.data(), bad_strategy.size())) == ids::kPayloadMalformed);
        std::vector<std::uint8_t> bad_status = frame;
        bad_status[first + 4] = 6;
        CHECK(error_code(decode_frame(bad_status.data(), bad_status.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("invalid UTF-8 in a name is refused by the encoder as well") {
        InstallHooks in = sample_install_hooks();
        in.hooks[0].name = "bad\xFF";
        Result<std::vector<std::uint8_t>> refused = encode_frame(in);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == ids::kPayloadMalformed);
    }
}

TEST_CASE("pseudo-random payloads never crash the hook decoders") {
    std::uint32_t state = 0x9E3779B9u;
    auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (int i = 0; i < 4000; ++i) {
        std::vector<std::uint8_t> payload(next() % 96);
        for (std::uint8_t& b : payload) {
            b = static_cast<std::uint8_t>(next());
        }
        for (std::uint16_t type : {static_cast<std::uint16_t>(0x0040), static_cast<std::uint16_t>(0x0041)}) {
            const Result<Message> r = decode_message(type, kMessageVersion, payload.data(), payload.size());
            if (r.has_value()) {
                CHECK(encode_frame(*r).has_value());
            }
        }
    }
}

TEST_CASE("hook messages are implemented, InstallHooks is a request, and the id comes first") {
    CHECK(is_implemented(MessageType::InstallHooks));
    CHECK(is_implemented(MessageType::HooksInstalled));
    CHECK(is_request_type(static_cast<std::uint16_t>(MessageType::InstallHooks)));
    CHECK_FALSE(is_request_type(static_cast<std::uint16_t>(MessageType::HooksInstalled)));
    const std::vector<std::uint8_t> frame = frame_bytes(sample_install_hooks());
    const auto id = peek_request_id(0x0040, frame.data() + kFrameHeaderSize, frame.size() - kFrameHeaderSize);
    REQUIRE(id.has_value());
    CHECK(*id == 0x1122334455667788ull);
    const std::vector<std::uint8_t> reply = frame_bytes(sample_hooks_installed());
    const auto reply_id = peek_request_id(0x0041, reply.data() + kFrameHeaderSize, reply.size() - kFrameHeaderSize);
    REQUIRE(reply_id.has_value());
    CHECK(*reply_id == 77);
    const std::string text = describe(sample_install_hooks());
    CHECK(text.find("InstallHooks") == 0);
    CHECK(text.find("hooks=3") != std::string::npos);
    InstallHooks revert = sample_install_hooks();
    revert.flags = kInstallHooksRevert;
    CHECK(describe(revert).find("(revert)") != std::string::npos);
    CHECK(describe(sample_hooks_installed()).find("status=4305") != std::string::npos);
    CHECK(std::string(to_string(MessageType::InstallHooks)) == "InstallHooks");
}
