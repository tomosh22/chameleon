#include "hook_plan.h"

#include "diag_ids.h"
#include "length_decoder.h"

namespace cml::agent::hook {

const char* to_string(Strategy strategy) noexcept {
    switch (strategy) {
        case Strategy::None: return "none";
        case Strategy::PaddingTwoByte: return "padding+2-byte";
        case Strategy::FiveByte: return "5-byte";
    }
    return "?";
}

bool encode_jmp_rel32(std::uint8_t out[5], std::uint64_t site, std::uint64_t target) noexcept {
    // Two's-complement difference computed in unsigned arithmetic, then range-checked as signed.
    const std::int64_t displacement = static_cast<std::int64_t>(target - (site + kJmpRel32Size));
    if (displacement < INT32_MIN || displacement > INT32_MAX) {
        return false;
    }
    const auto rel = static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
    out[0] = 0xE9;
    out[1] = static_cast<std::uint8_t>(rel);
    out[2] = static_cast<std::uint8_t>(rel >> 8);
    out[3] = static_cast<std::uint8_t>(rel >> 16);
    out[4] = static_cast<std::uint8_t>(rel >> 24);
    return true;
}

namespace {

HookPlan fail(std::uint32_t diag, const std::uint8_t* at, std::size_t at_len, std::size_t offset) noexcept {
    HookPlan plan;
    plan.diag = diag;
    plan.fail_offset = static_cast<std::uint32_t>(offset);
    for (std::size_t i = 0; i < 4 && offset + i < at_len; ++i) {
        plan.fail_bytes[i] = at[offset + i];
        plan.fail_byte_count = static_cast<std::uint8_t>(i + 1);
    }
    return plan;
}

std::uint32_t diag_for(DecodeStatus status) noexcept {
    // Truncated means the readable bytes ended inside an instruction.
    return status == DecodeStatus::Truncated ? ids::kCodeUnreadable : ids::kDecodeUnknown;
}

}  // namespace

HookPlan plan_hook(const std::uint8_t* before, std::size_t before_len, const std::uint8_t* at,
                   std::size_t at_len) noexcept {
    if (at == nullptr || at_len == 0) {
        return fail(ids::kCodeUnreadable, at, 0, 0);
    }

    const DecodeResult first = decode_instruction(at, at_len);
    if (first.status != DecodeStatus::Ok) {
        return fail(diag_for(first.status), at, at_len, 0);
    }

    // Strategy 1: padding in front, first instruction at least as long as the 2-byte jump.
    bool padded = before != nullptr && before_len >= kMinPadding;
    for (std::size_t i = 0; padded && i < kMinPadding; ++i) {
        padded = is_padding_byte(before[before_len - 1 - i]);
    }
    if (padded && first.length >= kShortJmpSize) {
        HookPlan plan;
        plan.strategy = Strategy::PaddingTwoByte;
        plan.first_insn_len = first.length;
        plan.entry_write_len = static_cast<std::uint32_t>(kShortJmpSize);
        plan.padding_write_len = static_cast<std::uint32_t>(kJmpRel32Size);
        plan.window_before = static_cast<std::uint32_t>(kJmpRel32Size);
        plan.window_after = first.length;  // >= 2, so it covers the 2 bytes written
        return plan;
    }

    // Strategy 2: own whole instructions until at least 5 bytes are covered.
    std::size_t covered = 0;
    std::size_t call_ends[kJmpRel32Size];  // a call is at least 2 bytes, so at most 2 fit below 5; be generous
    std::size_t call_count = 0;
    bool short_function = false;
    while (covered < kJmpRel32Size) {
        const DecodeResult insn = decode_instruction(at + covered, at_len - covered);
        if (insn.status != DecodeStatus::Ok) {
            return fail(diag_for(insn.status), at, at_len, covered);
        }
        const std::size_t instruction_start = covered;
        covered += insn.length;
        if ((insn.flags & kInsnCall) != 0 && call_count < kJmpRel32Size) {
            call_ends[call_count++] = covered;
        }
        if (ends_flow(insn.flags) && covered < kJmpRel32Size) {
            // The function ends before the jump does: the rest must be alignment padding that
            // nothing can execute.
            if (at_len < kJmpRel32Size) {
                return fail(ids::kCodeUnreadable, at, at_len, covered);
            }
            for (std::size_t i = covered; i < kJmpRel32Size; ++i) {
                if (!is_padding_byte(at[i])) {
                    return fail(ids::kFunctionTooShort, at, at_len, instruction_start);
                }
            }
            covered = kJmpRel32Size;
            short_function = true;
            break;
        }
    }
    // A call that ends inside the overwritten range: its callee would return into the jump.
    for (std::size_t i = 0; i < call_count; ++i) {
        if (call_ends[i] < covered) {
            return fail(ids::kCallInWindow, at, at_len, call_ends[i]);
        }
    }

    HookPlan plan;
    plan.strategy = Strategy::FiveByte;
    plan.first_insn_len = first.length;
    plan.entry_write_len = static_cast<std::uint32_t>(kJmpRel32Size);
    plan.window_after = static_cast<std::uint32_t>(covered);
    plan.short_function = short_function;
    return plan;
}

}  // namespace cml::agent::hook
