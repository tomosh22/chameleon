#include "cml/core/imgrel_site.h"

#include <algorithm>

#include "cml/util/format.h"
// The agent's instruction-length decoder, compiled into the broker unchanged (ADR-0042 decision 3). The include
// directory is given to this one source file by broker/cml_core/CMakeLists.txt.
#include "hook/length_decoder.h"

namespace cml::core::bind {

namespace lde = cml::agent::hook;

namespace {

constexpr std::uint32_t kMaxInstruction = 15;

}  // namespace

const char* to_string(SiteVerdict v) noexcept {
    switch (v) {
        case SiteVerdict::Displacement: return "displacement";
        case SiteVerdict::NotCode: return "not-code";
        case SiteVerdict::OutOfSection: return "out-of-section";
        case SiteVerdict::NoFunction: return "no-function";
        case SiteVerdict::Undecodable: return "undecodable";
        case SiteVerdict::RanIntoData: return "ran-into-data";
        case SiteVerdict::NotDisplacement: return "not-displacement";
        case SiteVerdict::RipRelative: return "rip-relative";
        case SiteVerdict::Address32: return "address-size-32";
        case SiteVerdict::SegmentFsGs: return "segment-fs-gs";
    }
    return "?";
}

SiteProver::SiteProver(bool is_code, std::span<const std::byte> data, std::vector<RelocField> fields, std::vector<std::uint32_t> function_starts)
    : is_code_(is_code), data_(data), fields_(std::move(fields)), starts_(std::move(function_starts)) {
    std::stable_sort(fields_.begin(), fields_.end(), [](const RelocField& a, const RelocField& b) { return a.offset < b.offset; });
    std::sort(starts_.begin(), starts_.end());
    starts_.erase(std::unique(starts_.begin(), starts_.end()), starts_.end());
}

std::string SiteProver::hex_bytes(std::uint32_t offset, std::uint32_t count) const {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (std::uint32_t i = 0; i < count && static_cast<std::uint64_t>(offset) + i < data_.size(); ++i) {
        const std::uint8_t b = std::to_integer<std::uint8_t>(data_[offset + i]);
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0xF]);
    }
    return out;
}

const SiteProver::Sweep& SiteProver::sweep_from(std::uint32_t start) {
    const auto found = sweeps_.find(start);
    if (found != sweeps_.end()) {
        return found->second;
    }
    Sweep s;
    // The function ends where the next one starts (or with the section).
    std::uint64_t limit = data_.size();
    const auto next = std::upper_bound(starts_.begin(), starts_.end(), start);
    if (next != starts_.end()) {
        limit = std::min<std::uint64_t>(limit, *next);
    }
    const std::uint8_t* const base = reinterpret_cast<const std::uint8_t*>(data_.data());
    std::size_t fi = static_cast<std::size_t>(
        std::lower_bound(fields_.begin(), fields_.end(), start, [](const RelocField& f, std::uint32_t v) { return f.offset < v; }) - fields_.begin());
    std::uint64_t off = start;
    bool stopped = false;
    // A relocation that begins before the function and reaches into it: the function does not start on a boundary.
    if (fi > 0 && static_cast<std::uint64_t>(fields_[fi - 1].offset) + fields_[fi - 1].width > start) {
        s.stop = SiteVerdict::RanIntoData;
        s.stop_reason = "the relocation at +" + hex(fields_[fi - 1].offset) + " reaches into the function that starts at +" + hex(start);
        stopped = true;
    }
    while (!stopped && off < limit) {
        const lde::DecodeResult r = lde::decode_instruction(base + off, static_cast<std::size_t>(limit - off));
        if (r.status != lde::DecodeStatus::Ok) {
            s.stop = SiteVerdict::Undecodable;
            const std::string bytes = hex_bytes(static_cast<std::uint32_t>(off), static_cast<std::uint32_t>(std::min<std::uint64_t>(kMaxInstruction, limit - off)));
            s.stop_reason = r.status == lde::DecodeStatus::Truncated
                                ? "the instruction at +" + hex(off) + " (" + bytes + ") runs past the end of its function"
                                : "the instruction at +" + hex(off) + " (" + bytes + ") is not known to the instruction decoder, so nothing behind it can be proved";
            stopped = true;
            break;
        }
        const std::uint64_t end = off + r.length;
        const std::uint64_t disp_lo = off + r.disp_offset;
        const std::uint64_t disp_hi = disp_lo + r.disp_size;
        const std::uint64_t tail_lo = r.disp_size != 0 ? disp_hi : off + 1;
        for (; fi < fields_.size() && fields_[fi].offset < end; ++fi) {
            const RelocField& f = fields_[fi];
            const std::uint64_t f_end = static_cast<std::uint64_t>(f.offset) + f.width;
            const bool in_displacement = f.width != 0 && r.disp_size != 0 && f.offset >= disp_lo && f_end <= disp_hi;
            const bool in_immediate = f.width != 0 && f.offset >= tail_lo && f_end == end;
            if (!in_displacement && !in_immediate) {
                s.stop = SiteVerdict::RanIntoData;
                s.stop_length = r.length;
                s.stop_reason = "the relocation at +" + hex(f.offset) + " does not fit an operand of the instruction decoded at +" + hex(off) + " (" +
                                hex_bytes(static_cast<std::uint32_t>(off), r.length) +
                                "): the bytes there are data (a jump table) or the instruction boundaries are lost";
                stopped = true;
                break;
            }
        }
        if (stopped) {
            break;
        }
        Insn insn;
        insn.start = static_cast<std::uint32_t>(off);
        insn.length = r.length;
        insn.flags = r.flags;
        insn.disp_offset = r.disp_offset;
        insn.disp_size = r.disp_size;
        s.insns.push_back(insn);
        off = end;
    }
    s.end = static_cast<std::uint32_t>(off);
    s.complete = !stopped;
    return sweeps_.emplace(start, std::move(s)).first->second;
}

SiteCheck SiteProver::check(std::uint32_t site) {
    SiteCheck c;
    c.at = site;
    if (!is_code_) {
        c.verdict = SiteVerdict::NotCode;
        c.bytes = hex_bytes(site, 4);
        c.reason = "the reference is in a data section, not in an instruction (bytes at the site: " + c.bytes + ")";
        return c;
    }
    if (static_cast<std::uint64_t>(site) + 4 > data_.size()) {
        c.verdict = SiteVerdict::OutOfSection;
        c.reason = "the site at +" + hex(site) + " is not inside the section data";
        return c;
    }
    const auto after = std::upper_bound(starts_.begin(), starts_.end(), site);
    if (after == starts_.begin()) {
        c.verdict = SiteVerdict::NoFunction;
        c.bytes = hex_bytes(site, 4);
        c.reason = "no function symbol precedes the site at +" + hex(site) + " in its section, so there is no instruction boundary to decode from";
        return c;
    }
    c.function_start = *(after - 1);
    const Sweep& s = sweep_from(c.function_start);
    if (site >= s.end) {
        // The sweep never got as far as the site's instruction.
        c.at = s.end;
        c.length = s.stop_length;
        c.bytes = hex_bytes(s.end, s.stop_length != 0 ? s.stop_length : kMaxInstruction);
        if (s.complete) {
            c.verdict = SiteVerdict::Undecodable;  // cannot happen: the site lies before the next function start
            c.reason = "the site at +" + hex(site) + " lies behind the decoded instructions of its function";
        } else {
            c.verdict = s.stop;
            c.reason = s.stop_reason;
        }
        return c;
    }
    // The instruction that holds the site: the last one that starts at or before it (the trusted ones are contiguous).
    auto it = std::upper_bound(s.insns.begin(), s.insns.end(), site, [](std::uint32_t v, const Insn& i) { return v < i.start; });
    const Insn& insn = *(it - 1);
    c.at = insn.start;
    c.length = insn.length;
    c.bytes = hex_bytes(insn.start, insn.length);
    const std::string where = "the instruction at +" + hex(insn.start) + " (" + c.bytes + ")";
    if (insn.disp_size != 4 || insn.start + insn.disp_offset != site) {
        c.verdict = SiteVerdict::NotDisplacement;
        const bool behind = insn.disp_size == 0 || site >= static_cast<std::uint32_t>(insn.start + insn.disp_offset + insn.disp_size);
        c.reason = "the site at +" + hex(site) + " is not the 32-bit displacement of a memory operand of " + where +
                   (behind ? ": it is an immediate operand" : "");
        return c;
    }
    if ((insn.flags & lde::kInsnRipRel) != 0) {
        c.verdict = SiteVerdict::RipRelative;
        c.reason = where + " addresses it RIP-relatively";
        return c;
    }
    if ((insn.flags & lde::kInsnAddr32) != 0) {
        c.verdict = SiteVerdict::Address32;
        c.reason = where + " has a 67 prefix: its address is truncated to 32 bits";
        return c;
    }
    if ((insn.flags & lde::kInsnSegFsGs) != 0) {
        c.verdict = SiteVerdict::SegmentFsGs;
        c.reason = where + " has an fs/gs segment override: its address is relative to that segment";
        return c;
    }
    c.verdict = SiteVerdict::Displacement;
    c.reason = "the 32-bit displacement of " + where;
    return c;
}

}  // namespace cml::core::bind
