// Wire format of CompileEvent (0x0070), WP-17. Portable (no Win32).
#include "cml/ipc/messages.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "cml/proc/diag_ids.h"
#include "doctest.h"

using namespace cml;
using namespace cml::ipc;

namespace {

std::vector<std::uint8_t> frame_bytes(const Message& m) {
    Result<std::vector<std::uint8_t>> frame = encode_frame(m);
    REQUIRE(frame.has_value());
    return *frame;
}

}  // namespace

TEST_CASE("CompileEvent: type id, round trip of every phase, event semantics") {
    CHECK(static_cast<int>(MessageType::CompileEvent) == 0x0070);
    CHECK(std::string(to_string(MessageType::CompileEvent)) == "CompileEvent");
    CHECK(is_implemented(MessageType::CompileEvent));
    CHECK_FALSE(is_request_type(static_cast<std::uint16_t>(MessageType::CompileEvent)));  // nothing answers it

    for (const CompilePhase phase : {CompilePhase::Start, CompilePhase::Success, CompilePhase::Error}) {
        CompileEvent in;
        in.phase = phase;
        in.diag = phase == CompilePhase::Error ? 2200 : 0;
        in.module = "game.dll";
        in.text = phase == CompilePhase::Error ? "logic.cpp(12): error C2065: 'x': undeclared identifier" : "";
        const std::vector<std::uint8_t> frame = frame_bytes(in);
        Result<Message> out = decode_frame(frame.data(), frame.size());
        REQUIRE(out.has_value());
        const auto* event = std::get_if<CompileEvent>(&*out);
        REQUIRE(event != nullptr);
        CHECK(event->phase == in.phase);
        CHECK(event->diag == in.diag);
        CHECK(event->module == in.module);
        CHECK(event->text == in.text);
        CHECK(type_of(*out) == MessageType::CompileEvent);
        CHECK_FALSE(describe(*out).empty());
        CHECK_FALSE(peek_request_id(0x0070, frame.data() + kFrameHeaderSize, frame.size() - kFrameHeaderSize).has_value());
    }
}

TEST_CASE("CompileEvent: unknown phase, truncation, trailing bytes and invalid UTF-8 are rejected") {
    CompileEvent in;
    in.phase = CompilePhase::Error;
    in.diag = 2200;
    in.module = "m";
    in.text = "t";
    std::vector<std::uint8_t> frame = frame_bytes(in);

    SUBCASE("phase 0 and phase 4") {
        for (const std::uint8_t bad : {std::uint8_t{0}, std::uint8_t{4}}) {
            std::vector<std::uint8_t> copy = frame;
            copy[kFrameHeaderSize] = bad;
            Result<Message> out = decode_frame(copy.data(), copy.size());
            REQUIRE_FALSE(out.has_value());
            CHECK(out.error().code == ids::kPayloadMalformed);
        }
    }
    SUBCASE("every truncation of the payload") {
        for (std::size_t cut = 1; cut < frame.size() - kFrameHeaderSize; ++cut) {
            Result<Message> out = decode_message(0x0070, kMessageVersion, frame.data() + kFrameHeaderSize,
                                                 frame.size() - kFrameHeaderSize - cut);
            CHECK_FALSE(out.has_value());
        }
    }
    SUBCASE("trailing bytes") {
        frame.push_back(0);
        Result<Message> out = decode_message(0x0070, kMessageVersion, frame.data() + kFrameHeaderSize,
                                             frame.size() - kFrameHeaderSize);
        REQUIRE_FALSE(out.has_value());
        CHECK(out.error().code == ids::kPayloadMalformed);
    }
    SUBCASE("the encoder refuses text that is not UTF-8") {
        in.text = "bad \xFF byte";
        CHECK_FALSE(encode_frame(in).has_value());
    }
}
