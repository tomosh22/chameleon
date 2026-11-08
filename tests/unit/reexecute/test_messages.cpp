// Wire format of the WP-23 additions: the frames appended to Exception, and the Reexecute request (0x00B0). Portable.
#include <cstdint>
#include <variant>
#include <vector>

#include "cml/ipc/messages.h"
#include "doctest.h"

using namespace cml;
using namespace cml::ipc;

namespace {

Exception sample_exception(std::size_t frames) {
    Exception e;
    e.exception_id = 3;
    e.thread_id = 99;
    e.code = 0xC0000005u;
    e.address = 0x7FF600001040ull;
    e.module_base = 0x7FF600000000ull;
    e.module_path = "C:\\b\\fixture_crash_reexecute.exe";
    for (std::size_t i = 0; i < frames; ++i) {
        ExceptionFrame f;
        f.rip = 0x7FF600001000ull + i;
        f.entry_rsp = 0x1000 + 0x100 * i;
        f.rsp = 0x0FD8 + 0x100 * i;
        f.rbp = 0x1000 + 0x100 * i;
        f.r13 = 0x13 + i;
        f.function_start = 0x7FF600001000ull;
        e.frames.push_back(f);
    }
    return e;
}

template <class T>
T round_trip(const T& message) {
    const Result<std::vector<std::uint8_t>> frame = encode_frame(message);
    REQUIRE(frame.has_value());
    const Result<Message> decoded = decode_frame(frame->data(), frame->size());
    REQUIRE_MESSAGE(decoded.has_value(), decoded.error().format());
    const T* back = std::get_if<T>(&*decoded);
    REQUIRE(back != nullptr);
    return *back;
}

}  // namespace

TEST_CASE("Exception: the frames of the held thread survive the wire, innermost first") {
    const Exception back = round_trip(sample_exception(3));
    REQUIRE(back.frames.size() == 3);
    CHECK(back.frames[0].rip == 0x7FF600001000ull);
    CHECK(back.frames[1].rip == 0x7FF600001001ull);
    CHECK(back.frames[2].entry_rsp == 0x1200);
    CHECK(back.frames[2].rsp == 0x0FD8 + 0x200);
    CHECK(back.frames[1].rbp == 0x1100);
    CHECK(back.frames[1].r13 == 0x14);
    CHECK(back.frames[0].function_start == 0x7FF600001000ull);
    CHECK(back.module_path == "C:\\b\\fixture_crash_reexecute.exe");
}

TEST_CASE("Exception: an Exception without frames is the WP-20 layout, byte for byte") {
    const Exception without = sample_exception(0);
    const Exception back = round_trip(without);
    CHECK(back.frames.empty());
    const Result<std::vector<std::uint8_t>> a = encode_frame(without);
    const Result<std::vector<std::uint8_t>> b = encode_frame(sample_exception(1));
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(b->size() == a->size() + 4 + 6 * 8);  // count + one frame
}

TEST_CASE("Exception: a lying frame count is rejected, not allocated") {
    Result<std::vector<std::uint8_t>> frame = encode_frame(sample_exception(1));
    REQUIRE(frame.has_value());
    // The frame count is the u32 right after module_path: header(8) + 4*5 + 8*4 + (4 + path).
    const std::size_t count_at = 8 + 20 + 32 + 4 + sample_exception(0).module_path.size();
    REQUIRE(frame->size() > count_at + 4);
    (*frame)[count_at] = 0xFF;  // 255 frames announced, one present
    const Result<Message> decoded = decode_frame(frame->data(), frame->size());
    CHECK_FALSE(decoded.has_value());
    (*frame)[count_at] = 65;  // above kMaxExceptionFrames
    CHECK_FALSE(decode_frame(frame->data(), frame->size()).has_value());
}

TEST_CASE("Reexecute: the recipe survives the wire") {
    Reexecute r;
    r.request_id = 41;
    r.exception_id = 3;
    r.frame_index = 1;
    r.entry_rsp = 0x1100;
    r.target = 0x7FF6DEAD0000ull;
    r.params.push_back(ReexecuteParam{0, 0, 8, 335, 0x30});
    r.params.push_back(ReexecuteParam{1, 0, 4, 334, -12});
    r.params.push_back(ReexecuteParam{2, 1, 8, 341, -24});
    const Reexecute back = round_trip(r);
    CHECK(back.request_id == 41);
    CHECK(back.exception_id == 3);
    CHECK(back.frame_index == 1);
    CHECK(back.entry_rsp == 0x1100);
    CHECK(back.target == 0x7FF6DEAD0000ull);
    REQUIRE(back.params.size() == 3);
    CHECK(back.params[1].offset == -12);
    CHECK(back.params[1].base_register == 334);
    CHECK(back.params[2].is_float == 1);
    CHECK(back.params[2].size == 8);
    CHECK(type_of(Message(r)) == MessageType::Reexecute);
    CHECK(static_cast<std::uint16_t>(MessageType::Reexecute) == 0x00B0);
    CHECK(std::string(to_string(MessageType::Reexecute)) == "Reexecute");
    CHECK(is_implemented(MessageType::Reexecute));
    CHECK(is_request_type(0x00B0));
    CHECK(describe(Message(r)).find("Reexecute request=41") == 0);
}

TEST_CASE("Reexecute: out-of-range parameters and counts are rejected") {
    Reexecute r;
    r.request_id = 1;
    r.params.push_back(ReexecuteParam{4, 0, 8, 335, 0});  // position 4 does not exist
    CHECK_FALSE(decode_frame(encode_frame(r)->data(), encode_frame(r)->size()).has_value());
    r.params[0] = ReexecuteParam{0, 2, 8, 335, 0};  // is_float 2
    CHECK_FALSE(decode_frame(encode_frame(r)->data(), encode_frame(r)->size()).has_value());
    r.params[0] = ReexecuteParam{0, 0, 3, 335, 0};  // size 3
    CHECK_FALSE(decode_frame(encode_frame(r)->data(), encode_frame(r)->size()).has_value());
    r.params.assign(5, ReexecuteParam{0, 0, 8, 335, 0});  // five parameters
    CHECK_FALSE(decode_frame(encode_frame(r)->data(), encode_frame(r)->size()).has_value());
    r.params.assign(4, ReexecuteParam{0, 0, 8, 335, 0});
    CHECK(decode_frame(encode_frame(r)->data(), encode_frame(r)->size()).has_value());
}
