// Wire format of Exception (0x0050), ExceptionAction (0x0051) and ExceptionConfig (0x0052), WP-20. Portable (no Win32).
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

std::uint32_t error_code(const Result<Message>& r) {
    return r.has_value() ? 0u : r.error().code;
}

}  // namespace

TEST_CASE("Exception: type id, round trip, event semantics") {
    CHECK(static_cast<int>(MessageType::Exception) == 0x0050);
    CHECK(std::string(to_string(MessageType::Exception)) == "Exception");
    CHECK(is_implemented(MessageType::Exception));
    CHECK_FALSE(is_request_type(0x0050));  // nothing answers it
    Exception in;
    in.exception_id = 7;
    in.thread_id = 4321;
    in.code = 0xC0000005u;
    in.flags = kExceptionFlagInPatch;
    in.access = 1;
    in.address = 0x00007FF612345678ull;
    in.data_address = 0x10;
    in.stack_pointer = 0x000000A1B2C3D4E8ull;
    in.module_base = 0x00007FF612340000ull;
    in.module_path = "C:\\tmp\\fixture_crash.exe";
    const std::vector<std::uint8_t> frame = frame_bytes(in);
    Result<Message> out = decode_frame(frame.data(), frame.size());
    REQUIRE(out.has_value());
    const auto* got = std::get_if<Exception>(&*out);
    REQUIRE(got != nullptr);
    CHECK(got->exception_id == 7);
    CHECK(got->thread_id == 4321);
    CHECK(got->code == 0xC0000005u);
    CHECK(got->flags == kExceptionFlagInPatch);
    CHECK(got->access == 1);
    CHECK(got->address == in.address);
    CHECK(got->data_address == 0x10);
    CHECK(got->stack_pointer == in.stack_pointer);
    CHECK(got->module_base == in.module_base);
    CHECK(got->module_path == in.module_path);
    CHECK(type_of(*out) == MessageType::Exception);
    CHECK_FALSE(describe(*out).empty());
    CHECK_FALSE(peek_request_id(0x0050, frame.data() + kFrameHeaderSize, frame.size() - kFrameHeaderSize).has_value());
}

TEST_CASE("Exception: truncation, trailing bytes and invalid UTF-8 are rejected") {
    Exception in;
    in.exception_id = 1;
    in.module_path = "m.dll";
    std::vector<std::uint8_t> frame = frame_bytes(in);
    SUBCASE("truncated payload") {
        frame.pop_back();
        frame[0] = static_cast<std::uint8_t>(frame[0] - 1);  // keep the header consistent
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("trailing byte") {
        frame.push_back(0);
        frame[0] = static_cast<std::uint8_t>(frame[0] + 1);
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("invalid UTF-8 never reaches the wire") {
        in.module_path = "bad\xFF";
        CHECK_FALSE(encode_frame(in).has_value());
    }
}

TEST_CASE("ExceptionAction: type id, round trip of every action, request semantics") {
    CHECK(static_cast<int>(MessageType::ExceptionAction) == 0x0051);
    CHECK(is_implemented(MessageType::ExceptionAction));
    CHECK(is_request_type(0x0051));
    for (const ExceptionActionKind action : {ExceptionActionKind::Continue, ExceptionActionKind::Leave, ExceptionActionKind::Ignore}) {
        ExceptionAction in;
        in.request_id = 0x1122334455667788ull;
        in.exception_id = 9;
        in.action = action;
        const std::vector<std::uint8_t> frame = frame_bytes(in);
        Result<Message> out = decode_frame(frame.data(), frame.size());
        REQUIRE(out.has_value());
        const auto* got = std::get_if<ExceptionAction>(&*out);
        REQUIRE(got != nullptr);
        CHECK(got->request_id == in.request_id);
        CHECK(got->exception_id == 9);
        CHECK(got->action == action);
        CHECK(type_of(*out) == MessageType::ExceptionAction);
        CHECK_FALSE(describe(*out).empty());
        const std::optional<std::uint64_t> id = peek_request_id(0x0051, frame.data() + kFrameHeaderSize, frame.size() - kFrameHeaderSize);
        REQUIRE(id.has_value());
        CHECK(*id == in.request_id);
    }
    SUBCASE("an unknown action is rejected") {
        ExceptionAction in;
        in.action = static_cast<ExceptionActionKind>(4);
        const std::vector<std::uint8_t> frame = frame_bytes(in);
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        in.action = static_cast<ExceptionActionKind>(0);
        const std::vector<std::uint8_t> zero = frame_bytes(in);
        CHECK(error_code(decode_frame(zero.data(), zero.size())) == ids::kPayloadMalformed);
    }
}

TEST_CASE("ExceptionConfig: type id, round trip, unknown flags rejected") {
    CHECK(static_cast<int>(MessageType::ExceptionConfig) == 0x0052);
    CHECK(is_implemented(MessageType::ExceptionConfig));
    CHECK(is_request_type(0x0052));
    ExceptionConfig in;
    in.request_id = 12;
    in.flags = kExceptionConfigEnabled | kExceptionConfigBreakWithDebugger;
    const std::vector<std::uint8_t> frame = frame_bytes(in);
    Result<Message> out = decode_frame(frame.data(), frame.size());
    REQUIRE(out.has_value());
    const auto* got = std::get_if<ExceptionConfig>(&*out);
    REQUIRE(got != nullptr);
    CHECK(got->request_id == 12);
    CHECK(got->flags == in.flags);
    CHECK(type_of(*out) == MessageType::ExceptionConfig);
    CHECK_FALSE(describe(*out).empty());
    in.flags = 4;
    const std::vector<std::uint8_t> bad = frame_bytes(in);
    CHECK(error_code(decode_frame(bad.data(), bad.size())) == ids::kPayloadMalformed);
}
