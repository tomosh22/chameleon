#include "cml/core/hook_narrowing.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <unordered_map>

#include "cml/util/log.h"

namespace cml::core {

const char* to_string(NarrowReason reason) noexcept {
    switch (reason) {
        case NarrowReason::Unchanged: return "unchanged";
        case NarrowReason::BytesDiffer: return "bytes-differ";
        case NarrowReason::SizeDiffers: return "size-differs";
        case NarrowReason::UnknownOldSize: return "unknown-old-size";
        case NarrowReason::OldBytesUnavailable: return "old-bytes-unavailable";
        case NarrowReason::AlreadyHooked: return "already-hooked";
        case NarrowReason::TargetChanged: return "target-changed";
        case NarrowReason::TargetNew: return "target-new";
        case NarrowReason::Unverifiable: return "unverifiable";
        case NarrowReason::Disabled: return "narrowing-off";
    }
    return "?";
}

namespace {

constexpr std::uint64_t kMaxConstantBytes = 64 * 1024;  // larger constants are not compared: the function is kept

std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

// Width in bytes of the field a relocation of this type patches; 0 = a type this check cannot judge.
std::uint32_t reloc_width(coff::RelocType type) {
    switch (type) {
        case coff::RelocType::Addr64: return 8;
        case coff::RelocType::Addr32:
        case coff::RelocType::Addr32Nb:
        case coff::RelocType::Rel32:
        case coff::RelocType::Rel32_1:
        case coff::RelocType::Rel32_2:
        case coff::RelocType::Rel32_3:
        case coff::RelocType::Rel32_4:
        case coff::RelocType::Rel32_5:
        case coff::RelocType::SecRel:
        case coff::RelocType::SRel32: return 4;
        case coff::RelocType::Section: return 2;
        default: return 0;
    }
}

std::uint32_t rel32_n(coff::RelocType type) {
    return static_cast<std::uint32_t>(static_cast<std::uint16_t>(type) - static_cast<std::uint16_t>(coff::RelocType::Rel32));
}

std::uint64_t load_le(const std::byte* p, std::uint32_t width) {
    std::uint64_t v = 0;
    for (std::uint32_t i = 0; i < width; ++i) {
        v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(p[i])) << (8u * i);
    }
    return v;
}

std::int64_t sign_extend32(std::uint64_t v) {
    return static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(v)));
}

struct Keep {
    NarrowReason reason;
    std::string detail;
};

class Narrower {
public:
    explicit Narrower(const NarrowInput& in) : in_(in) {
        for (const bind::SymbolDecision& d : in_.plan->decisions) {
            by_raw_index_.emplace(d.symbol_index, &d);
        }
        for (const bind::Fixup& f : in_.plan->fixups) {
            if (f.kind == bind::FixupKind::SecRel32) {
                secrel_[(static_cast<std::uint64_t>(f.section_index) << 32) | f.offset] = &f;
            }
        }
    }

    NarrowDecision judge(const bind::HookEntry& h) const {
        NarrowDecision out;
        out.symbol = h.new_symbol;
        out.display = h.display;
        if (!in_.enabled) {
            out.reason = NarrowReason::Disabled;
            return out;
        }
        if (const std::optional<Keep> keep = check(h)) {
            out.reason = keep->reason;
            out.detail = keep->detail;
        } else {
            out.reason = NarrowReason::Unchanged;
        }
        return out;
    }

private:
    // nullopt: identical, drop the hook.
    std::optional<Keep> check(const bind::HookEntry& h) const {
        if (in_.old_image == nullptr || in_.object == nullptr) {
            return Keep{NarrowReason::OldBytesUnavailable, "no module image"};
        }
        if (h.old_rva == bind::kNoRva) {
            return Keep{NarrowReason::OldBytesUnavailable, "the old entry is not in the module file (defined by an earlier patch)"};
        }
        if (in_.is_hooked && in_.is_hooked(h.old_va)) {
            return Keep{NarrowReason::AlreadyHooked, "the entry currently redirects to an earlier patch"};
        }
        if (h.old_size == 0) {
            return Keep{NarrowReason::UnknownOldSize, "the PDB gave no size"};
        }
        const auto dec = by_raw_name(h.new_symbol);
        if (dec == nullptr || dec->section == 0) {
            return Keep{NarrowReason::Unverifiable, "no section for the definition"};
        }
        const coff::Section* sec = in_.object->section(dec->section);
        if (sec == nullptr || sec->data.empty()) {
            return Keep{NarrowReason::Unverifiable, "no section data"};
        }
        // Extent of the new function inside its section.
        std::uint64_t start = dec->value;
        std::uint64_t len = sec->data.size() - std::min<std::uint64_t>(start, sec->data.size());
        if (!sec->is_comdat()) {
            len = extent_in_section(*sec, dec->section, dec->value);
        }
        if (len != h.old_size) {
            return Keep{NarrowReason::SizeDiffers, "new " + std::to_string(len) + " bytes, old " + std::to_string(h.old_size)};
        }
        if (start + len > sec->data.size()) {
            return Keep{NarrowReason::Unverifiable, "the function leaves its section"};
        }
        Result<pe::ByteSpan> old_bytes = in_.old_image->read_rva(h.old_rva, len);
        if (!old_bytes) {
            return Keep{NarrowReason::OldBytesUnavailable, old_bytes.error().message};
        }
        Result<std::vector<coff::ResolvedRelocation>> relocs = in_.object->resolve_relocations(*sec);
        if (!relocs) {
            return Keep{NarrowReason::Unverifiable, relocs.error().message};
        }
        std::vector<std::uint8_t> masked(len, 0);
        std::vector<const coff::ResolvedRelocation*> inside;
        for (const coff::ResolvedRelocation& r : *relocs) {
            if (r.reloc.virtual_address < start || r.reloc.virtual_address >= start + len) {
                continue;
            }
            const std::uint32_t w = reloc_width(r.reloc.type);
            if (w == 0) {
                return Keep{NarrowReason::Unverifiable, std::string("relocation type ") + std::string(coff::reloc_type_name(r.reloc.type))};
            }
            const std::uint64_t at = r.reloc.virtual_address - start;
            if (at + w > len) {
                return Keep{NarrowReason::Unverifiable, "a relocation crosses the end of the function"};
            }
            std::memset(masked.data() + at, 1, w);
            inside.push_back(&r);
        }
        for (std::uint64_t i = 0; i < len; ++i) {
            if (masked[i] == 0 && sec->data[start + i] != (*old_bytes)[i]) {
                return Keep{NarrowReason::BytesDiffer, "byte " + std::to_string(i) + " differs"};
            }
        }
        for (const coff::ResolvedRelocation* r : inside) {
            if (std::optional<Keep> keep = check_target(h, *sec, dec->section, start, *r, *old_bytes)) {
                return keep;
            }
        }
        return std::nullopt;
    }

    std::optional<Keep> check_target(const bind::HookEntry& h, const coff::Section& sec, std::uint32_t sec_index, std::uint64_t start,
                                     const coff::ResolvedRelocation& r, pe::ByteSpan old_bytes) const {
        const std::string name(r.target->name);
        const auto it = by_raw_index_.find(r.reloc.symbol_index);
        if (it == by_raw_index_.end()) {
            return Keep{NarrowReason::Unverifiable, "no decision for '" + name + "'"};
        }
        const bind::SymbolDecision& d = *it->second;
        const std::uint64_t site = r.reloc.virtual_address;     // offset in the object section
        const std::uint64_t at = site - start;                  // offset in the function
        const std::uint32_t width = reloc_width(r.reloc.type);
        if (site + width > sec.data.size()) {
            return Keep{NarrowReason::Unverifiable, "relocation outside the section data"};
        }
        const std::uint64_t old_raw = load_le(old_bytes.data() + at, width);
        const std::uint64_t new_raw = load_le(sec.data.data() + site, width);  // the in-place addend A

        if (r.reloc.type == coff::RelocType::SecRel) {
            const auto f = secrel_.find((static_cast<std::uint64_t>(sec_index) << 32) | static_cast<std::uint32_t>(site));
            if (f == secrel_.end()) {
                return Keep{NarrowReason::Unverifiable, "SECREL to '" + name + "' is not a planned fix-up"};
            }
            const std::int64_t expect = static_cast<std::int64_t>(f->second->target_section_offset) + f->second->addend;
            if (static_cast<std::int64_t>(old_raw) != expect) {
                return Keep{NarrowReason::TargetChanged, "SECREL to '" + name + "' is " + hex(old_raw) + " in the old code, " + hex(static_cast<std::uint64_t>(expect)) + " now"};
            }
            return std::nullopt;
        }

        // The address the old code reached through this relocation.
        std::uint64_t old_target = 0;
        if (bind::is_rel32(rel32_kind_of(r.reloc.type))) {
            const std::uint64_t old_site_va = in_.load_base + h.old_rva + at;
            old_target = old_site_va + 4u + rel32_n(r.reloc.type) + static_cast<std::uint64_t>(sign_extend32(old_raw)) - static_cast<std::uint64_t>(sign_extend32(new_raw));
        } else if (r.reloc.type == coff::RelocType::Addr64) {
            const std::uint64_t preferred = in_.old_image->optional_header().image_base;
            old_target = old_raw + (in_.load_base - preferred) - new_raw;
        } else {
            return Keep{NarrowReason::Unverifiable, std::string(coff::reloc_type_name(r.reloc.type)) + " to '" + name + "' in code"};
        }

        switch (d.decision) {
            case bind::DecisionKind::BindOldData:
            case bind::DecisionKind::BindOldFunction:
            case bind::DecisionKind::PatchHooked: {
                if (!d.old_address.valid) {
                    return Keep{NarrowReason::Unverifiable, "'" + name + "' has no old address"};
                }
                if (old_target != d.old_address.va) {
                    return Keep{NarrowReason::TargetChanged, "reference to '" + d.display + "' reached " + hex(old_target) + " in the old code, now " + hex(d.old_address.va)};
                }
                return std::nullopt;
            }
            case bind::DecisionKind::LinkerProvided:
                if (old_target != in_.load_base) {
                    return Keep{NarrowReason::TargetChanged, "__ImageBase reference differs"};
                }
                return std::nullopt;
            case bind::DecisionKind::PatchNewFunction:
            case bind::DecisionKind::PatchNewData:
                return Keep{NarrowReason::TargetNew, "reference to the new symbol '" + d.display + "'"};
            case bind::DecisionKind::PatchFresh:
                return check_fresh_constant(d, old_target);
            default:
                return Keep{NarrowReason::Unverifiable, "reference to '" + name + "' (" + bind::to_string(d.decision) + ")"};
        }
    }

    static bind::FixupKind rel32_kind_of(coff::RelocType type) {
        switch (type) {
            case coff::RelocType::Rel32: return bind::FixupKind::Rel32;
            case coff::RelocType::Rel32_1: return bind::FixupKind::Rel32_1;
            case coff::RelocType::Rel32_2: return bind::FixupKind::Rel32_2;
            case coff::RelocType::Rel32_3: return bind::FixupKind::Rel32_3;
            case coff::RelocType::Rel32_4: return bind::FixupKind::Rel32_4;
            case coff::RelocType::Rel32_5: return bind::FixupKind::Rel32_5;
            default: return bind::FixupKind::Slot;  // "not rel32"
        }
    }

    // A read-only constant the patch carries as a fresh copy: equal iff the bytes at the old target are equal.
    std::optional<Keep> check_fresh_constant(const bind::SymbolDecision& d, std::uint64_t old_target) const {
        if (d.type != bind::SymbolType::Data || !d.defined || d.section == 0) {
            return Keep{NarrowReason::TargetChanged, "reference to the fresh copy of '" + d.display + "'"};
        }
        const coff::Section* tsec = in_.object->section(d.section);
        if (tsec == nullptr || tsec->is_code() || (tsec->characteristics & coff::scn::MemWrite) != 0 || tsec->data.empty()) {
            return Keep{NarrowReason::TargetNew, "'" + d.display + "' is not read-only constant data"};
        }
        const std::uint64_t len = tsec->is_comdat() ? tsec->data.size() - std::min<std::uint64_t>(d.value, tsec->data.size())
                                                    : extent_in_section(*tsec, d.section, d.value);
        if (len == 0 || len > kMaxConstantBytes || d.value + len > tsec->data.size()) {
            return Keep{NarrowReason::Unverifiable, "extent of '" + d.display + "' unknown"};
        }
        if (old_target < in_.load_base || old_target - in_.load_base > 0xFFFFFFFFull) {
            return Keep{NarrowReason::TargetChanged, "'" + d.display + "' was reached outside the module in the old code"};
        }
        Result<pe::ByteSpan> old_const = in_.old_image->read_rva(static_cast<std::uint32_t>(old_target - in_.load_base), len);
        if (!old_const) {
            return Keep{NarrowReason::TargetChanged, "the old constant for '" + d.display + "' is not readable"};
        }
        // A constant that holds pointers (vtables, jump tables, function tables) is not judged: where its
        // relocations point cannot be compared, so the function that uses it is kept.
        Result<std::vector<coff::ResolvedRelocation>> relocs = in_.object->resolve_relocations(*tsec);
        if (!relocs) {
            return Keep{NarrowReason::Unverifiable, relocs.error().message};
        }
        for (const coff::ResolvedRelocation& rr : *relocs) {
            if (rr.reloc.virtual_address >= d.value && rr.reloc.virtual_address < d.value + len) {
                return Keep{NarrowReason::Unverifiable, "the constant '" + d.display + "' holds pointers"};
            }
        }
        for (std::uint64_t i = 0; i < len; ++i) {
            if (tsec->data[d.value + i] != (*old_const)[i]) {
                return Keep{NarrowReason::TargetChanged, "the constant '" + d.display + "' differs at byte " + std::to_string(i)};
            }
        }
        return std::nullopt;
    }

    // Bytes from `value` to the next symbol defined in the same section (or the section end).
    std::uint64_t extent_in_section(const coff::Section& sec, std::uint32_t sec_index, std::uint32_t value) const {
        std::uint64_t end = sec.data.size();
        for (const coff::Symbol& s : in_.object->symbols()) {
            if (s.section_number != static_cast<std::int32_t>(sec_index) || s.value <= value) {
                continue;
            }
            end = std::min<std::uint64_t>(end, s.value);
        }
        return end > value ? end - value : 0;
    }

    const bind::SymbolDecision* by_raw_name(const std::string& name) const {
        return in_.plan->find_decision(name);
    }

    const NarrowInput& in_;
    std::unordered_map<std::uint32_t, const bind::SymbolDecision*> by_raw_index_;
    std::unordered_map<std::uint64_t, const bind::Fixup*> secrel_;
};

}  // namespace

NarrowResult narrow_hooks(const NarrowInput& input) {
    NarrowResult out;
    if (input.plan == nullptr) {
        return out;
    }
    const Narrower narrower(input);
    out.decisions.reserve(input.plan->hooks.size());
    for (const bind::HookEntry& h : input.plan->hooks) {
        NarrowDecision d = narrower.judge(h);
        CML_LOG(Trace, "narrow: %s -> %s%s%s", h.display.c_str(), to_string(d.reason), d.detail.empty() ? "" : ": ", d.detail.c_str());
        if (is_dropped(d.reason)) {
            ++out.dropped;
        } else {
            out.hooks.push_back(h);
        }
        out.decisions.push_back(std::move(d));
    }
    return out;
}

}  // namespace cml::core
