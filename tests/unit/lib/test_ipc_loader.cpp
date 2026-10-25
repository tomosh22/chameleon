// WP-08 wire messages: address-space reservation and patch loading (portable, no Win32).
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include "cml/ipc/messages.h"
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

Message round_trip(const Message& in) {
    const std::vector<std::uint8_t> frame = frame_bytes(in);
    Result<Message> out = decode_frame(frame.data(), frame.size());
    REQUIRE_MESSAGE(out.has_value(), (out.has_value() ? "" : out.error().format()));
    CHECK(type_of(*out) == type_of(in));
    return *out;
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

Fixup make_fixup(std::uint32_t rva, FixupKind kind, std::uint8_t n, std::uint64_t value) {
    Fixup f;
    f.rva = rva;
    f.kind = kind;
    f.n = n;
    f.value = value;
    return f;
}

LoadPatch sample_load_patch() {
    LoadPatch in;
    in.request_id = 11;
    in.patch_id = 0xABCDEF0123ull;
    for (std::uint32_t i = 0; i < 3; ++i) {
        PatchCandidate c;
        c.path = "C:\\tmp\\patch_caf\xC3\xA9_" + std::to_string(i) + ".dll";
        c.image_base = 0x00000066'00000000ull + i * 0x10000;
        c.image_size = 0x6000 + i;
        c.fixups.push_back(make_fixup(0x1004, FixupKind::Rel32, 0, 0x7FF6'12345678ull));
        c.fixups.push_back(make_fixup(0x2008, FixupKind::Rel32, 5, 0x1));
        c.fixups.push_back(make_fixup(0x3000, FixupKind::Addr64, 0, 0xFFFFFFFF'FFFFFFFFull));
        c.fixups.push_back(make_fixup(0x3010, FixupKind::SlotWrite, 0, 0x7FF6'00001000ull));
        in.candidates.push_back(std::move(c));
    }
    in.initializer_rvas = {0x4000, 0x4008, 0x4010};
    return in;
}

}  // namespace

TEST_CASE("WP-08 messages round-trip") {
    SUBCASE("ReserveRange request") {
        ReserveRange in;
        in.request_id = 5;
        in.module_base = 0x7FF6'00000000ull;
        in.module_size = 0x123000;
        in.block_size = 4ull << 20;
        in.reach = 1ull << 30;
        in.block_count = 7;
        const auto out = std::get<ReserveRange>(round_trip(in));
        CHECK(out.request_id == 5);
        CHECK_FALSE(out.answer);
        CHECK(out.module_base == in.module_base);
        CHECK(out.module_size == in.module_size);
        CHECK(out.block_size == in.block_size);
        CHECK(out.reach == in.reach);
        CHECK(out.block_count == 7);
    }
    SUBCASE("ReserveRange answer") {
        ReserveRange in;
        in.request_id = 5;
        in.answer = true;
        in.status = 4201;
        in.message = "only one block";
        in.blocks.push_back({0x7FF5'FC00'0000ull, 4ull << 20});
        in.blocks.push_back({0x7FF5'F800'0000ull, 4ull << 20});
        const auto out = std::get<ReserveRange>(round_trip(in));
        CHECK(out.answer);
        CHECK(out.status == 4201);
        CHECK(out.message == "only one block");
        REQUIRE(out.blocks.size() == 2);
        CHECK(out.blocks[1].base == 0x7FF5'F800'0000ull);
        CHECK(out.blocks[1].size == (4ull << 20));
    }
    SUBCASE("ReleaseRange") {
        ReleaseRange in;
        in.request_id = 6;
        in.base = 0x1000'0000;
        in.size = 0x40000;
        in.mode = ReleaseMode::UnloadPatch;
        const auto out = std::get<ReleaseRange>(round_trip(in));
        CHECK(out.base == in.base);
        CHECK(out.size == in.size);
        CHECK(out.mode == ReleaseMode::UnloadPatch);
    }
    SUBCASE("LoadPatch") {
        const LoadPatch in = sample_load_patch();
        const auto out = std::get<LoadPatch>(round_trip(in));
        CHECK(out.request_id == in.request_id);
        CHECK(out.patch_id == in.patch_id);
        REQUIRE(out.candidates.size() == 3);
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(out.candidates[i].path == in.candidates[i].path);
            CHECK(out.candidates[i].image_base == in.candidates[i].image_base);
            CHECK(out.candidates[i].image_size == in.candidates[i].image_size);
            REQUIRE(out.candidates[i].fixups.size() == 4);
            CHECK(out.candidates[i].fixups[1].kind == FixupKind::Rel32);
            CHECK(out.candidates[i].fixups[1].n == 5);
            CHECK(out.candidates[i].fixups[2].value == 0xFFFFFFFF'FFFFFFFFull);
            CHECK(out.candidates[i].fixups[3].kind == FixupKind::SlotWrite);
        }
        CHECK(out.initializer_rvas == in.initializer_rvas);
    }
    SUBCASE("PatchLoaded") {
        PatchLoaded in;
        in.request_id = 11;
        in.patch_id = 99;
        in.status = 4203;
        in.message = "range taken";
        in.image_base = 0x66'00010000ull;
        in.image_size = 0x6000;
        in.candidate_index = 2;
        in.attempts = 3;
        in.fixups_applied = 17;
        in.initializers_run = 2;
        const auto out = std::get<PatchLoaded>(round_trip(in));
        CHECK(out.patch_id == 99);
        CHECK(out.status == 4203);
        CHECK(out.message == "range taken");
        CHECK(out.image_base == in.image_base);
        CHECK(out.image_size == 0x6000);
        CHECK(out.candidate_index == 2);
        CHECK(out.attempts == 3);
        CHECK(out.fixups_applied == 17);
        CHECK(out.initializers_run == 2);
    }
    SUBCASE("ApplyFixups and RunInitializers") {
        ApplyFixups fixups;
        fixups.request_id = 8;
        fixups.image_base = 0x66'00000000ull;
        fixups.fixups = {make_fixup(8, FixupKind::Addr64, 0, 1), make_fixup(16, FixupKind::Rel32, 3, 2)};
        const auto f = std::get<ApplyFixups>(round_trip(fixups));
        CHECK(f.image_base == fixups.image_base);
        REQUIRE(f.fixups.size() == 2);
        CHECK(f.fixups[1].n == 3);

        RunInitializers inits;
        inits.request_id = 9;
        inits.image_base = 0x66'00000000ull;
        inits.rvas = {0x10, 0x20};
        const auto i = std::get<RunInitializers>(round_trip(inits));
        CHECK(i.rvas == inits.rvas);
    }
    SUBCASE("empty lists are legal") {
        ApplyFixups empty;
        CHECK(std::get<ApplyFixups>(round_trip(empty)).fixups.empty());
        RunInitializers none;
        CHECK(std::get<RunInitializers>(round_trip(none)).rvas.empty());
    }
}

TEST_CASE("WP-08 fix-up wire layout is fixed") {
    ApplyFixups m;
    m.request_id = 1;
    m.image_base = 2;
    m.fixups = {make_fixup(0x11223344, FixupKind::Rel32, 4, 0x0102030405060708ull)};
    const std::vector<std::uint8_t> frame = frame_bytes(m);
    // 8 (id) + 8 (base) + 4 (count) + 16 (fix-up)
    REQUIRE(frame.size() == kFrameHeaderSize + 36);
    const std::uint8_t expected_fixup[16] = {0x44, 0x33, 0x22, 0x11,  // rva
                                             1,                      // kind Rel32
                                             4,                      // n
                                             0, 0,                   // reserved
                                             8, 7, 6, 5, 4, 3, 2, 1};
    CHECK(std::memcmp(frame.data() + kFrameHeaderSize + 20, expected_fixup, 16) == 0);
}

TEST_CASE("WP-08 malformed payloads are diagnosed") {
    const std::vector<std::uint8_t> good = frame_bytes(sample_load_patch());
    // Offsets inside the payload of LoadPatch: id (8), patch id (8), flags (4), candidate count (4).
    const std::size_t flags_at = kFrameHeaderSize + 16;
    const std::size_t count_at = kFrameHeaderSize + 20;
    SUBCASE("non-zero reserved flags") {
        std::vector<std::uint8_t> frame = good;
        frame[flags_at] = 1;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("zero candidates") {
        LoadPatch in = sample_load_patch();
        in.candidates.clear();
        const std::vector<std::uint8_t> frame = frame_bytes(in);
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("more than five candidates") {
        LoadPatch in = sample_load_patch();
        while (in.candidates.size() < 6) {
            in.candidates.push_back(in.candidates.front());
        }
        const std::vector<std::uint8_t> frame = frame_bytes(in);
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("a lying candidate count") {
        std::vector<std::uint8_t> frame = good;
        frame[count_at] = 5;  // 3 candidates present
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("a lying fix-up count cannot force a huge allocation") {
        ApplyFixups m;
        std::vector<std::uint8_t> frame = frame_bytes(m);
        frame[kFrameHeaderSize + 16] = 0xFF;
        frame[kFrameHeaderSize + 17] = 0xFF;
        frame[kFrameHeaderSize + 18] = 0xFF;
        frame[kFrameHeaderSize + 19] = 0x7F;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        frame[kFrameHeaderSize + 16] = 0x10;  // 16 fix-ups claimed, none present
        frame[kFrameHeaderSize + 17] = 0;
        frame[kFrameHeaderSize + 18] = 0;
        frame[kFrameHeaderSize + 19] = 0;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("unknown fix-up kind, bad n, non-zero reserved bytes") {
        ApplyFixups m;
        m.fixups = {make_fixup(8, FixupKind::Rel32, 1, 5)};
        const std::vector<std::uint8_t> base = frame_bytes(m);
        const std::size_t fixup_at = kFrameHeaderSize + 20;
        std::vector<std::uint8_t> frame = base;
        frame[fixup_at + 4] = 0;  // kind 0
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        frame = base;
        frame[fixup_at + 4] = 4;  // kind 4 (ADDR32NB never travels)
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        frame = base;
        frame[fixup_at + 5] = 6;  // n = 6
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        frame = base;
        frame[fixup_at + 4] = 2;  // Addr64 with n = 1
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        frame = base;
        frame[fixup_at + 7] = 1;  // reserved
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("ReserveRange: bad form, too many blocks") {
        ReserveRange m;
        std::vector<std::uint8_t> frame = frame_bytes(m);
        frame[kFrameHeaderSize + 8] = 2;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
        ReserveRange many;
        many.block_count = 65;
        frame = frame_bytes(many);
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("ReleaseRange: unknown mode") {
        ReleaseRange m;
        std::vector<std::uint8_t> frame = frame_bytes(m);
        frame[kFrameHeaderSize + 24] = 2;
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
    SUBCASE("every truncation of a LoadPatch is rejected, never read out of range") {
        const std::size_t payload = good.size() - kFrameHeaderSize;
        for (std::size_t cut = 0; cut < payload; ++cut) {
            Result<Message> r = decode_message(static_cast<std::uint16_t>(MessageType::LoadPatch), kMessageVersion,
                                               good.data() + kFrameHeaderSize, cut);
            CHECK_FALSE(r.has_value());
        }
    }
    SUBCASE("trailing bytes") {
        std::vector<std::uint8_t> frame = good;
        frame.push_back(0);
        set_payload_size(frame, static_cast<std::uint32_t>(frame.size() - kFrameHeaderSize));
        CHECK(error_code(decode_frame(frame.data(), frame.size())) == ids::kPayloadMalformed);
    }
}

TEST_CASE("WP-08 request classification and request-id peeking") {
    for (const MessageType t : {MessageType::ReserveRange, MessageType::ReleaseRange, MessageType::LoadPatch,
                                MessageType::ApplyFixups, MessageType::RunInitializers}) {
        CHECK(is_request_type(static_cast<std::uint16_t>(t)));
        CHECK(is_implemented(t));
    }
    CHECK_FALSE(is_request_type(static_cast<std::uint16_t>(MessageType::PatchLoaded)));
    CHECK(is_implemented(MessageType::PatchLoaded));
    const std::vector<std::uint8_t> frame = frame_bytes(sample_load_patch());
    const auto id = peek_request_id(static_cast<std::uint16_t>(MessageType::LoadPatch),
                                    frame.data() + kFrameHeaderSize, frame.size() - kFrameHeaderSize);
    REQUIRE(id.has_value());
    CHECK(*id == 11);
    CHECK(peek_request_id(static_cast<std::uint16_t>(MessageType::PatchLoaded), frame.data() + kFrameHeaderSize, 8)
              .has_value());
}

TEST_CASE("WP-08 describe names the key fields") {
    const std::string text = describe(sample_load_patch());
    CHECK(text.find("LoadPatch") != std::string::npos);
    CHECK(text.find("candidates=3") != std::string::npos);
    CHECK(text.find("initializers=3") != std::string::npos);
    ReserveRange answer;
    answer.answer = true;
    CHECK(describe(answer).find("ReserveRange(answer)") != std::string::npos);
}

TEST_CASE("WP-08 the per-message fix-up limit is enforced by the decoder") {
    LoadPatch in = sample_load_patch();
    in.candidates.resize(1);
    in.candidates[0].fixups.assign(kMaxFixupsPerMessage, make_fixup(8, FixupKind::Addr64, 0, 1));
    // 60000 * 16 = 960000 bytes: fits the 1 MiB frame and the limit.
    Result<std::vector<std::uint8_t>> ok_frame = encode_frame(in);
    REQUIRE(ok_frame.has_value());
    CHECK(decode_frame(ok_frame->data(), ok_frame->size()).has_value());
    in.candidates[0].fixups.assign(kMaxFixupsPerMessage + 1, make_fixup(8, FixupKind::Addr64, 0, 1));
    Result<std::vector<std::uint8_t>> too_many = encode_frame(in);
    if (too_many.has_value()) {
        CHECK(error_code(decode_frame(too_many->data(), too_many->size())) == ids::kPayloadMalformed);
    } else {
        CHECK(too_many.error().code == ids::kMessageTooLarge);
    }
}
