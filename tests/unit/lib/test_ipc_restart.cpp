// Wire format of the hot-restart messages (WP-21): Restart 0x0061, RestartInfo 0x0062, ReenableModules 0x0063 and
// the optional restart token at the end of Hello. Portable (no Win32).
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
    REQUIRE_FALSE(r.has_value());
    return r.error().code;
}

RestartInfo sample_info() {
    RestartInfo info;
    info.request_id = 77;
    info.hooks_run = 2;
    info.image_path = u"C:\\app\\game.exe";
    info.command_line = u"\"C:\\app\\game.exe\" --level \"a b\"";
    info.working_directory = u"C:\\app";
    info.environment = std::u16string(u"A=1\0PATH=C:\\x\0\0", 15);
    info.std_input = 0x44;
    info.std_output = 0x48;
    info.std_error = 0x48;
    info.modules.push_back(RestartModule{1, "C:\\app\\game.exe"});
    info.modules.push_back(RestartModule{0, "plugin.dll"});
    return info;
}

}  // namespace

TEST_CASE("Restart, RestartInfo and ReenableModules: type ids, names, request semantics") {
    CHECK(static_cast<int>(MessageType::Restart) == 0x0061);
    CHECK(static_cast<int>(MessageType::RestartInfo) == 0x0062);
    CHECK(static_cast<int>(MessageType::ReenableModules) == 0x0063);
    CHECK(std::string(to_string(MessageType::Restart)) == "Restart");
    CHECK(std::string(to_string(MessageType::RestartInfo)) == "RestartInfo");
    CHECK(std::string(to_string(MessageType::ReenableModules)) == "ReenableModules");
    for (const MessageType t : {MessageType::Restart, MessageType::RestartInfo, MessageType::ReenableModules}) {
        CHECK(is_implemented(t));
    }
    // The broker sends Restart and ReenableModules and waits for an answer; RestartInfo is an answer.
    CHECK(is_request_type(0x0061));
    CHECK(is_request_type(0x0063));
    CHECK_FALSE(is_request_type(0x0062));
    CHECK(MessageType::Log == static_cast<MessageType>(0x0060));  // unchanged by this package
}

TEST_CASE("Restart round trip, request id readable from a damaged payload") {
    Restart in;
    in.request_id = 9001;
    const std::vector<std::uint8_t> frame = frame_bytes(in);
    Result<Message> out = decode_frame(frame.data(), frame.size());
    REQUIRE(out.has_value());
    const auto* restart = std::get_if<Restart>(&*out);
    REQUIRE(restart != nullptr);
    CHECK(restart->request_id == 9001);
    CHECK(restart->flags == 0);
    CHECK(peek_request_id(0x0061, frame.data() + kFrameHeaderSize, frame.size() - kFrameHeaderSize) == std::optional<std::uint64_t>(9001));
    CHECK_FALSE(describe(*out).empty());

    // Flags are reserved: a non-zero value is refused.
    std::vector<std::uint8_t> bad = frame;
    bad[kFrameHeaderSize + 8] = 1;
    CHECK(error_code(decode_frame(bad.data(), bad.size())) == ids::kPayloadMalformed);
}

TEST_CASE("RestartInfo round trip keeps wide text exactly, lone surrogates included") {
    RestartInfo in = sample_info();
    in.command_line.push_back(static_cast<char16_t>(0xD800));  // a lone surrogate: legal in a Windows command line
    const std::vector<std::uint8_t> frame = frame_bytes(in);
    Result<Message> out = decode_frame(frame.data(), frame.size());
    REQUIRE(out.has_value());
    const auto* info = std::get_if<RestartInfo>(&*out);
    REQUIRE(info != nullptr);
    CHECK(info->request_id == in.request_id);
    CHECK(info->status == 0);
    CHECK(info->hooks_run == 2);
    CHECK(info->image_path == in.image_path);
    CHECK(info->command_line == in.command_line);
    CHECK(info->working_directory == in.working_directory);
    CHECK(info->environment == in.environment);
    CHECK(info->environment.size() == 15);  // the embedded NULs survive
    CHECK(info->std_input == 0x44);
    CHECK(info->std_output == 0x48);
    CHECK(info->std_error == 0x48);
    REQUIRE(info->modules.size() == 2);
    CHECK(info->modules[0].options == 1);
    CHECK(info->modules[1].path == "plugin.dll");
    CHECK(peek_request_id(0x0062, frame.data() + kFrameHeaderSize, frame.size() - kFrameHeaderSize) == std::optional<std::uint64_t>(77));
    CHECK_FALSE(describe(*out).empty());
}

TEST_CASE("RestartInfo failure answer: status and message, empty parameters") {
    RestartInfo in;
    in.request_id = 5;
    in.status = 4450;
    in.message = "no launch information";
    const std::vector<std::uint8_t> frame = frame_bytes(in);
    Result<Message> out = decode_frame(frame.data(), frame.size());
    REQUIRE(out.has_value());
    const auto* info = std::get_if<RestartInfo>(&*out);
    REQUIRE(info != nullptr);
    CHECK(info->status == 4450);
    CHECK(info->message == "no launch information");
    CHECK(info->command_line.empty());
    CHECK(info->modules.empty());
}

TEST_CASE("RestartInfo: truncation, trailing bytes, lying counts and oversized wide fields are rejected") {
    const std::vector<std::uint8_t> good = frame_bytes(sample_info());

    SUBCASE("truncated payload") {
        for (const std::size_t cut : {std::size_t{1}, std::size_t{9}, std::size_t{40}}) {
            std::vector<std::uint8_t> frame(good.begin(), good.end() - static_cast<std::ptrdiff_t>(cut));
            const std::uint32_t size = static_cast<std::uint32_t>(frame.size() - kFrameHeaderSize);
            for (int i = 0; i < 4; ++i) {
                frame[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size >> (8 * i));
            }
            CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        }
    }
    SUBCASE("trailing bytes") {
        std::vector<std::uint8_t> frame = good;
        frame.push_back(0);
        frame[0] = static_cast<std::uint8_t>(frame.size() - kFrameHeaderSize);
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("a wide field that claims more units than the payload holds") {
        RestartInfo in;
        in.request_id = 1;
        std::vector<std::uint8_t> frame = frame_bytes(in);
        // request id (8) + status (4) + empty message (4) + hooks_run (4): the image_path unit count follows.
        const std::size_t at = kFrameHeaderSize + 8 + 4 + 4 + 4;
        frame[at] = 0xFF;
        frame[at + 1] = 0xFF;
        frame[at + 2] = 0x00;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("a lying module count") {
        RestartInfo in;
        in.request_id = 1;
        std::vector<std::uint8_t> frame = frame_bytes(in);
        frame[frame.size() - 4] = 0xFF;  // the module count is the last field
        frame[frame.size() - 3] = 0xFF;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("a wide field above the limit cannot be encoded") {
        RestartInfo in = sample_info();
        in.environment.assign(kMaxWideUnits + 1, u'x');
        Result<std::vector<std::uint8_t>> frame = encode_frame(in);
        REQUIRE_FALSE(frame.has_value());
        CHECK(frame.error().code == ids::kMessageTooLarge);
    }
    SUBCASE("a string that is not UTF-8 is not sent") {
        RestartInfo in = sample_info();
        in.modules[0].path = "\xC3\x28";
        Result<std::vector<std::uint8_t>> frame = encode_frame(in);
        REQUIRE_FALSE(frame.has_value());
        CHECK(frame.error().code == ids::kPayloadMalformed);
    }
}

TEST_CASE("ReenableModules round trip and limits") {
    ReenableModules in;
    in.request_id = 12;
    in.modules.push_back(RestartModule{1, "C:\\app\\a.dll"});
    in.modules.push_back(RestartModule{0, "b.dll"});
    const std::vector<std::uint8_t> frame = frame_bytes(in);
    Result<Message> out = decode_frame(frame.data(), frame.size());
    REQUIRE(out.has_value());
    const auto* got = std::get_if<ReenableModules>(&*out);
    REQUIRE(got != nullptr);
    CHECK(got->request_id == 12);
    REQUIRE(got->modules.size() == 2);
    CHECK(got->modules[0].path == "C:\\app\\a.dll");
    CHECK(got->modules[0].options == 1);
    CHECK(peek_request_id(0x0063, frame.data() + kFrameHeaderSize, frame.size() - kFrameHeaderSize) == std::optional<std::uint64_t>(12));
    CHECK_FALSE(describe(*out).empty());

    std::vector<std::uint8_t> lying = frame_bytes(ReenableModules{1, {}});
    lying[kFrameHeaderSize + 8] = 0xFF;  // module count
    lying[kFrameHeaderSize + 9] = 0x7F;
    CHECK(error_code(decode_frame(lying.data(), lying.size())) == ids::kPayloadMalformed);
}

TEST_CASE("Hello carries the restart token only when there is one, and old Hello frames still decode") {
    Hello plain;
    plain.request_id = 1;
    plain.pid = 100;
    plain.agent_path = "C:\\a\\cml_agent.dll";
    plain.command_line = "game.exe";
    const std::vector<std::uint8_t> plain_frame = frame_bytes(plain);
    Result<Message> plain_out = decode_frame(plain_frame.data(), plain_frame.size());
    REQUIRE(plain_out.has_value());
    CHECK(std::get<Hello>(*plain_out).restart_token.empty());

    Hello restarted = plain;
    restarted.restart_token = "0123456789abcdef0123456789abcdef";
    const std::vector<std::uint8_t> frame = frame_bytes(restarted);
    CHECK(frame.size() == plain_frame.size() + 4 + restarted.restart_token.size());
    Result<Message> out = decode_frame(frame.data(), frame.size());
    REQUIRE(out.has_value());
    CHECK(std::get<Hello>(*out).restart_token == restarted.restart_token);
    CHECK(describe(*out).find("restart_token") != std::string::npos);
    CHECK(describe(*plain_out).find("restart_token") == std::string::npos);

    // A token cut short is malformed, and so is garbage after it.
    std::vector<std::uint8_t> cut(frame.begin(), frame.end() - 3);
    const std::uint32_t size = static_cast<std::uint32_t>(cut.size() - kFrameHeaderSize);
    for (int i = 0; i < 4; ++i) {
        cut[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(size >> (8 * i));
    }
    CHECK(error_code(decode_frame(cut.data(), cut.size())) == ids::kPayloadMalformed);
    std::vector<std::uint8_t> extra = frame;
    extra.push_back(1);
    extra[0] = static_cast<std::uint8_t>(extra.size() - kFrameHeaderSize);
    CHECK(error_code(decode_frame(extra.data(), extra.size())) == ids::kPayloadMalformed);
}
