// Fuzz target: the agent <-> broker wire deserialiser, cml_ipc messages (WP-28). Every message type, both directions: the
// type is part of the input, so the mutator reaches each decoder.
//
// Input forms (the same bytes are used for all of them):
//   1. a whole frame (8-byte header + payload)                           decode_frame, parse_frame_header
//   2. [u16 type][u16 version][payload...]                               decode_message (header fields chosen freely)
//   3. the bytes as text                                                 UTF-8 validation and sanitising
// What decodes is encoded again and must decode to the same message (describe() text equal): an encoder that produces
// frames its own decoder rejects, or a decoder that is not deterministic, aborts the run.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "cml/ipc/messages.h"
#include "fuzz_common.h"

namespace {

[[noreturn]] void bug(const char* what) {
    std::fprintf(stderr, "fuzz_ipc: %s\n", what);
    std::abort();
}

void check_round_trip(const cml::ipc::Message& message) {
    using namespace cml;
    const std::string before = ipc::describe(message);
    fuzz::use(before.size());
    Result<std::vector<std::uint8_t>> frame = ipc::encode_frame(message);
    if (!frame) {
        return;  // a decoded message that cannot be encoded (limit) is a diagnostic, not a crash
    }
    Result<ipc::Message> again = ipc::decode_frame(frame->data(), frame->size());
    if (!again) {
        bug("a frame produced by encode_frame does not decode");
    }
    if (ipc::type_of(*again) != ipc::type_of(message) || ipc::describe(*again) != before) {
        bug("encode/decode round trip changed the message");
    }
    Result<std::vector<std::uint8_t>> frame2 = ipc::encode_frame(*again);
    if (!frame2 || *frame2 != *frame) {
        bug("re-encoding a decoded frame gives other bytes");
    }
}

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace cml;
    // 1. frame
    if (size >= ipc::kFrameHeaderSize) {
        fuzz::use(ipc::parse_frame_header(data, size) ? 1 : 0);
    }
    if (Result<ipc::Message> m = ipc::decode_frame(data, size)) {
        check_round_trip(*m);
    } else {
        fuzz::use(m.error().code);
    }
    // 2. type and version chosen by the input
    if (size >= 4) {
        const std::uint16_t type = le16(data);
        const std::uint16_t version = le16(data + 2);
        const std::uint8_t* payload = size > 4 ? data + 4 : nullptr;
        const std::size_t payload_size = size - 4;
        fuzz::use(ipc::is_request_type(type) ? 1 : 0);
        fuzz::use(ipc::peek_request_id(type, payload, payload_size).value_or(0));
        if (Result<ipc::Message> m = ipc::decode_message(type, version, payload, payload_size)) {
            check_round_trip(*m);
        }
        // Valid version: the decoder proper (version 1 is the only one).
        if (Result<ipc::Message> m = ipc::decode_message(type, ipc::kMessageVersion, payload, payload_size)) {
            check_round_trip(*m);
        }
    }
    // 3. text helpers
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const bool valid = ipc::is_valid_utf8(text);
    const std::string clean = ipc::sanitize_utf8(text, 300);
    if (!ipc::is_valid_utf8(clean) || clean.size() > 300) {
        bug("sanitize_utf8 returned invalid UTF-8 or too many bytes");
    }
    if (valid && size <= 300 && clean != text) {
        bug("sanitize_utf8 changed valid UTF-8 that fits");
    }
    const std::size_t cut = ipc::utf8_cut_point(text, size / 2);
    if (cut > size / 2) {
        bug("utf8_cut_point is past the limit");
    }
    return 0;
}
