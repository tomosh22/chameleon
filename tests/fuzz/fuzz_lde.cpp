// Fuzz target: the hook installer's instruction-length decoder and the hook planner on top of it (WP-38, ADR-0042).
// Input = the bytes at a function entry (or any bytes).
//
// The decoder's own invariants, checked in every build (libFuzzer + ASan in fuzz.yml, plain replay of the seeds in
// every `ci` cell):
//   * it never reads past `available`: every prefix of the input is decoded from a heap block of exactly that size
//     (AddressSanitizer faults on an over-read) and from two stack copies whose bytes behind the prefix differ (the
//     results must be equal in any build);
//   * Ok means a length of 1..15 that fits the bytes, a displacement of 0, 1 or 4 bytes lying inside the instruction
//     behind its first byte, kInsnRipRel only with a 4-byte displacement, at most one control-flow kind;
//     Unknown and Truncated carry no length, flags or displacement;
//   * the verdict does not depend on what follows the instruction: with fewer bytes than the length it is Truncated,
//     with exactly the length or more it is the same Ok, and an Unknown never turns into something else when more
//     bytes become visible;
//   * plan_hook() never claims more than it was given and its window covers what it writes.
// With CML_LDE_REFERENCE (a tree configured with CML_ZYDIS_DIR; fuzz.yml) every decoded position is also compared
// with Zydis: lde_reference.h has the rule. A violation prints the bytes and aborts.
//
// When the process ends normally it prints what it did (the evidence line of fuzz.yml; a disagreement would have
// aborted before it):
//   WP38-FUZZ reference=zydis inputs=N positions=P ok=A agree=B exceptions=E disagreements=0
//   WP38-FUZZ reference=none inputs=N positions=P ok=A            (no CML_ZYDIS_DIR: invariants only)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "fuzz_common.h"
#include "hook/hook_plan.h"
#include "hook/length_decoder.h"

#if defined(CML_LDE_REFERENCE)
#include "lde_reference.h"
#endif

namespace {

using namespace cml::agent::hook;

[[noreturn]] void bug(const char* what, const std::uint8_t* bytes, std::size_t count) {
    std::fprintf(stderr, "fuzz_lde: %s\n  bytes:", what);
    for (std::size_t i = 0; i < count && i < 32; ++i) {
        std::fprintf(stderr, " %02x", bytes[i]);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    std::abort();
}

struct Counters {
    unsigned long long inputs = 0;
    unsigned long long positions = 0;
    unsigned long long ok = 0;
    unsigned long long agree = 0;
    unsigned long long exceptions = 0;
    ~Counters() {
#if defined(CML_LDE_REFERENCE)
        std::fprintf(stderr, "WP38-FUZZ reference=zydis inputs=%llu positions=%llu ok=%llu agree=%llu exceptions=%llu disagreements=0\n",
                     inputs, positions, ok, agree, exceptions);
#else
        std::fprintf(stderr, "WP38-FUZZ reference=none inputs=%llu positions=%llu ok=%llu\n", inputs, positions, ok);
#endif
    }
};
Counters g_counters;

bool same(const DecodeResult& a, const DecodeResult& b) noexcept {
    return a.status == b.status && a.length == b.length && a.flags == b.flags && a.disp_offset == b.disp_offset &&
           a.disp_size == b.disp_size;
}

// What a result may look like, whatever the input.
void check_shape(const DecodeResult& r, const std::uint8_t* bytes, std::size_t available) {
    if (r.status != DecodeStatus::Ok) {
        if (r.length != 0 || r.flags != 0 || r.disp_offset != 0 || r.disp_size != 0) {
            bug("a result that is not Ok carries a length, flags or a displacement", bytes, available);
        }
        return;
    }
    if (r.length < 1 || r.length > 15 || r.length > available) {
        bug("Ok with a length outside 1..15 or beyond the available bytes", bytes, available);
    }
    if (r.disp_size != 0 && r.disp_size != 1 && r.disp_size != 4) {
        bug("displacement size is not 0, 1 or 4", bytes, available);
    }
    if (r.disp_size == 0 && r.disp_offset != 0) {
        bug("displacement offset without a displacement", bytes, available);
    }
    // In front of a displacement there is at least an opcode and a ModRM byte.
    if (r.disp_size != 0 && (r.disp_offset < 2 || r.disp_offset + r.disp_size > r.length)) {
        bug("the displacement does not lie inside the instruction", bytes, available);
    }
    if ((r.flags & kInsnRipRel) != 0 && r.disp_size != 4) {
        bug("RIP-relative without a 32-bit displacement", bytes, available);
    }
    const unsigned flow = r.flags & (kInsnCall | kInsnJmp | kInsnJcc | kInsnRet | kInsnTrap);
    if ((flow & (flow - 1)) != 0) {
        bug("more than one control-flow kind", bytes, available);
    }
}

// decode_instruction() on exactly `count` bytes, three ways; the results must be one.
DecodeResult decode_guarded(const std::uint8_t* bytes, std::size_t count) {
    // A heap block of exactly `count` bytes: AddressSanitizer reports a read behind it.
    const std::unique_ptr<std::uint8_t[]> exact(new std::uint8_t[count == 0 ? 1 : count]);
    if (count != 0) {
        std::memcpy(exact.get(), bytes, count);
    }
    const DecodeResult r = decode_instruction(exact.get(), count);
    // Two copies that differ behind the prefix: a result that depends on those bytes shows in any build.
    std::uint8_t low[48];
    std::uint8_t high[48];
    std::memset(low, 0x00, sizeof(low));
    std::memset(high, 0xFF, sizeof(high));
    const std::size_t n = std::min<std::size_t>(count, 32);
    std::memcpy(low, bytes, n);
    std::memcpy(high, bytes, n);
    if (n == count) {
        if (!same(r, decode_instruction(low, count)) || !same(r, decode_instruction(high, count))) {
            bug("the verdict depends on bytes beyond `available`", bytes, count);
        }
    }
    return r;
}

void check_prefixes(const std::uint8_t* bytes, std::size_t size) {
    const std::size_t n = std::min<std::size_t>(size, 24);
    const DecodeResult full = decode_guarded(bytes, n);
    check_shape(full, bytes, n);
    bool seen_unknown = false;
    for (std::size_t k = 1; k <= n; ++k) {
        const DecodeResult r = decode_guarded(bytes, k);
        check_shape(r, bytes, k);
        if (full.status == DecodeStatus::Ok) {
            if (k < full.length ? r.status != DecodeStatus::Truncated : !same(r, full)) {
                bug("the verdict changes with the bytes that follow the instruction", bytes, n);
            }
        } else if (full.status == DecodeStatus::Truncated) {
            if (r.status != DecodeStatus::Truncated) {
                bug("a prefix of a truncated instruction is not Truncated", bytes, n);
            }
        } else {
            if (r.status == DecodeStatus::Ok || (seen_unknown && r.status != DecodeStatus::Unknown)) {
                bug("an Unknown verdict is not stable under more bytes", bytes, n);
            }
            seen_unknown = seen_unknown || r.status == DecodeStatus::Unknown;
        }
    }
    if (full.status == DecodeStatus::Ok) {
        // The same instruction followed by other bytes.
        std::uint8_t other[48];
        std::memset(other, 0xCC, sizeof(other));
        std::memcpy(other, bytes, full.length);
        for (std::size_t k = full.length; k < n; ++k) {
            other[k] = static_cast<std::uint8_t>(~bytes[k]);
        }
        if (!same(full, decode_instruction(other, sizeof(other)))) {
            bug("the verdict depends on what follows the instruction", bytes, n);
        }
    }
    if (decode_instruction(nullptr, size).status != DecodeStatus::Truncated ||
        decode_instruction(bytes, 0).status != DecodeStatus::Truncated) {
        bug("no bytes must be Truncated", bytes, n);
    }
}

void check_plan(const std::uint8_t* data, std::size_t size) {
    // The first byte picks how many of the following bytes are the bytes in front of the entry.
    if (size < 2) {
        return;
    }
    const std::size_t before_len = std::min<std::size_t>(data[0] % (kLookBehind + 1), size - 1);
    const std::uint8_t* before = data + 1;
    const std::uint8_t* at = before + before_len;
    const std::size_t at_len = std::min<std::size_t>(size - 1 - before_len, kLookAhead);
    PlanEnvironment environment;
    environment.entry_two_byte_aligned = (data[0] & 0x40) == 0;
    environment.padding_executable = (data[0] & 0x80) == 0;
    const HookPlan plan = plan_hook(before, before_len, at, at_len, environment);
    switch (plan.strategy) {
        case Strategy::None:
            if (plan.diag == 0 || plan.fail_byte_count > 4 || plan.fail_offset > at_len) {
                bug("a refused plan without a diagnostic, or with a culprit outside the bytes", at, at_len);
            }
            break;
        case Strategy::PaddingTwoByte:
            if (before_len < kMinPadding || plan.first_insn_len < kShortJmpSize || plan.entry_write_len != kShortJmpSize ||
                plan.padding_write_len != kJmpRel32Size || plan.window_before != kJmpRel32Size ||
                plan.window_after < kShortJmpSize || plan.window_after > at_len || !environment.entry_two_byte_aligned ||
                !environment.padding_executable) {
                bug("an impossible padding plan", at, at_len);
            }
            break;
        case Strategy::FiveByte:
            if (plan.entry_write_len != kJmpRel32Size || plan.padding_write_len != 0 || plan.window_before != 0 ||
                plan.window_after < kJmpRel32Size || plan.window_after > at_len || plan.window_after > kJmpRel32Size - 1 + 15) {
                bug("an impossible five-byte plan", at, at_len);
            }
            break;
    }
    cml::fuzz::use(plan.window_after);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    ++g_counters.inputs;
    check_prefixes(data, size);
    check_plan(data, size);

#if defined(CML_LDE_REFERENCE)
    static const cml::fuzz::lde::Reference reference;
#endif
    // The input as a stream of instructions: every boundary the decoder finds is one more decoded position.
    std::size_t at = 0;
    for (int count = 0; count < 64 && at < size; ++count) {
        const DecodeResult r = decode_instruction(data + at, size - at);
        check_shape(r, data + at, size - at);
        ++g_counters.positions;
        g_counters.ok += r.status == DecodeStatus::Ok ? 1u : 0u;
#if defined(CML_LDE_REFERENCE)
        const cml::fuzz::lde::Comparison c = reference.compare(data + at, size - at);
        g_counters.agree += c.verdict == cml::fuzz::lde::Verdict::Agree ? 1u : 0u;
        g_counters.exceptions += c.verdict == cml::fuzz::lde::Verdict::Exception ? 1u : 0u;
        if (c.verdict == cml::fuzz::lde::Verdict::Mismatch) {
            std::fprintf(stderr, "fuzz_lde: reference: ours=%u/%u reference=%s/%u\n", static_cast<unsigned>(c.ours.status),
                         static_cast<unsigned>(c.ours.length), cml::fuzz::lde::status_name(c.reference_status),
                         c.reference_length);
            bug(c.why, data + at, size - at);
        }
#endif
        if (r.status != DecodeStatus::Ok) {
            break;
        }
        cml::fuzz::use(r.length);
        at += r.length;
    }
    return 0;
}
