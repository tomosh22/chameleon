#include "cml/core/hook_narrowing.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <unordered_map>

#include "cml/util/format.h"
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

    std::optional<Keep> capture_code(const std::string& symbol, CodeImage* out) const { return capture(symbol, out); }

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
    // Where the new function lives in the new object.
    struct Loc {
        const bind::SymbolDecision* dec = nullptr;
        const coff::Section* sec = nullptr;
        std::uint64_t start = 0;  // offset in the section
        std::uint64_t len = 0;
    };

    std::optional<Keep> locate(const std::string& symbol, Loc* loc) const {
        loc->dec = by_raw_name(symbol);
        if (loc->dec == nullptr || loc->dec->section == 0) {
            return Keep{NarrowReason::Unverifiable, "no section for the definition"};
        }
        loc->sec = in_.object->section(loc->dec->section);
        if (loc->sec == nullptr || loc->sec->data.empty()) {
            return Keep{NarrowReason::Unverifiable, "no section data"};
        }
        // Extent of the new function inside its section.
        loc->start = loc->dec->value;
        loc->len = loc->sec->data.size() - std::min<std::uint64_t>(loc->start, loc->sec->data.size());
        if (!loc->sec->is_comdat()) {
            loc->len = extent_in_section(*loc->sec, loc->dec->section, loc->dec->value);
        }
        return std::nullopt;
    }

    // nullopt: identical, drop the hook.
    std::optional<Keep> check(const bind::HookEntry& h) const {
        if (in_.object == nullptr) {
            return Keep{NarrowReason::OldBytesUnavailable, "no module image"};
        }
        const bool hooked = in_.is_hooked && in_.is_hooked(h.old_va);
        if (hooked || h.old_rva == bind::kNoRva) {
            // WP-25: the entry runs code of an earlier patch (hooked, or defined by one): compare with what the broker recorded.
            if (in_.prior_code) {
                if (const std::shared_ptr<const CodeImage> prior = in_.prior_code(h.old_va)) {
                    return check_prior(h, *prior);
                }
            }
            if (h.old_rva == bind::kNoRva) {
                return Keep{NarrowReason::OldBytesUnavailable, "the old entry is not in the module file (defined by an earlier patch)"};
            }
            return Keep{NarrowReason::AlreadyHooked, "the entry currently redirects to an earlier patch"};
        }
        if (in_.old_image == nullptr) {
            return Keep{NarrowReason::OldBytesUnavailable, "no module image"};
        }
        if (h.old_size == 0) {
            return Keep{NarrowReason::UnknownOldSize, "the PDB gave no size"};
        }
        Loc loc;
        if (std::optional<Keep> keep = locate(h.new_symbol, &loc)) {
            return keep;
        }
        const bind::SymbolDecision* dec = loc.dec;
        const coff::Section* sec = loc.sec;
        const std::uint64_t start = loc.start;
        const std::uint64_t len = loc.len;
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

    // ---- WP-25: recorded code of earlier patches -----------------------------------------------------------------

    // Records the new function `symbol`: bytes with the relocation sites zeroed, and what every relocation reaches.
    std::optional<Keep> capture(const std::string& symbol, CodeImage* out) const {
        if (in_.object == nullptr || in_.plan == nullptr) {
            return Keep{NarrowReason::Unverifiable, "no object"};
        }
        Loc loc;
        if (std::optional<Keep> keep = locate(symbol, &loc)) {
            return keep;
        }
        const coff::Section& sec = *loc.sec;
        if (loc.len == 0 || loc.start + loc.len > sec.data.size()) {
            return Keep{NarrowReason::Unverifiable, "the function leaves its section"};
        }
        Result<std::vector<coff::ResolvedRelocation>> relocs = in_.object->resolve_relocations(sec);
        if (!relocs) {
            return Keep{NarrowReason::Unverifiable, relocs.error().message};
        }
        out->bytes.assign(loc.len, 0);
        for (std::uint64_t i = 0; i < loc.len; ++i) {
            out->bytes[i] = std::to_integer<std::uint8_t>(sec.data[loc.start + i]);
        }
        out->relocs.clear();
        for (const coff::ResolvedRelocation& r : *relocs) {
            const std::uint64_t site = r.reloc.virtual_address;
            if (site < loc.start || site >= loc.start + loc.len) {
                continue;
            }
            const std::uint32_t w = reloc_width(r.reloc.type);
            if (w == 0) {
                return Keep{NarrowReason::Unverifiable, std::string("relocation type ") + std::string(coff::reloc_type_name(r.reloc.type))};
            }
            const std::uint64_t at = site - loc.start;
            if (at + w > loc.len) {
                return Keep{NarrowReason::Unverifiable, "a relocation crosses the end of the function"};
            }
            CodeReloc cr;
            cr.offset = static_cast<std::uint32_t>(at);
            cr.type = static_cast<std::uint16_t>(r.reloc.type);
            cr.addend = load_le(sec.data.data() + site, w);
            if (std::optional<Keep> keep = capture_target(loc.dec->section, r, &cr.target)) {
                return keep;
            }
            std::memset(out->bytes.data() + at, 0, w);
            out->relocs.push_back(std::move(cr));
        }
        return std::nullopt;
    }

    std::optional<Keep> capture_target(std::uint32_t sec_index, const coff::ResolvedRelocation& r, CodeTarget* out) const {
        const std::string name(r.target->name);
        const auto it = by_raw_index_.find(r.reloc.symbol_index);
        if (it == by_raw_index_.end()) {
            return Keep{NarrowReason::Unverifiable, "no decision for '" + name + "'"};
        }
        const bind::SymbolDecision& d = *it->second;
        if (r.reloc.type == coff::RelocType::SecRel) {
            const auto f = secrel_.find((static_cast<std::uint64_t>(sec_index) << 32) | static_cast<std::uint32_t>(r.reloc.virtual_address));
            if (f == secrel_.end()) {
                return Keep{NarrowReason::Unverifiable, "SECREL to '" + name + "' is not a planned fix-up"};
            }
            out->kind = CodeTarget::Kind::SecRel;
            out->value = static_cast<std::uint64_t>(static_cast<std::int64_t>(f->second->target_section_offset) + f->second->addend);
            return std::nullopt;
        }
        if (!bind::is_rel32(rel32_kind_of(r.reloc.type)) && r.reloc.type != coff::RelocType::Addr64) {
            return Keep{NarrowReason::Unverifiable, std::string(coff::reloc_type_name(r.reloc.type)) + " to '" + name + "' in code"};
        }
        switch (d.decision) {
            case bind::DecisionKind::BindOldData:
            case bind::DecisionKind::BindOldFunction:
            case bind::DecisionKind::PatchHooked:
                if (!d.old_address.valid) {
                    return Keep{NarrowReason::Unverifiable, "'" + name + "' has no old address"};
                }
                out->kind = CodeTarget::Kind::Address;
                out->value = d.old_address.va;
                return std::nullopt;
            case bind::DecisionKind::LinkerProvided:
                out->kind = CodeTarget::Kind::Address;
                out->value = in_.load_base;
                return std::nullopt;
            case bind::DecisionKind::PatchNewFunction:
            case bind::DecisionKind::PatchNewData:
                out->kind = CodeTarget::Kind::NewSymbol;
                out->name = d.name;
                return std::nullopt;
            case bind::DecisionKind::PatchFresh: {
                const coff::Section* tsec = nullptr;
                std::uint64_t len = 0;
                if (std::optional<Keep> keep = fresh_extent(d, &tsec, &len)) {
                    return keep;
                }
                if (std::optional<Keep> keep = fresh_no_pointers(d, *tsec, len)) {
                    return keep;
                }
                out->kind = CodeTarget::Kind::Constant;
                out->bytes.resize(len);
                for (std::uint64_t i = 0; i < len; ++i) {
                    out->bytes[i] = std::to_integer<std::uint8_t>(tsec->data[d.value + i]);
                }
                return std::nullopt;
            }
            default:
                return Keep{NarrowReason::Unverifiable, "reference to '" + name + "' (" + bind::to_string(d.decision) + ")"};
        }
    }

    // The same three rules, against the code an earlier patch put at the entry.
    std::optional<Keep> check_prior(const bind::HookEntry& h, const CodeImage& prior) const {
        CodeImage now;
        if (std::optional<Keep> keep = capture(h.new_symbol, &now)) {
            return keep;
        }
        if (now.bytes.size() != prior.bytes.size()) {
            return Keep{NarrowReason::SizeDiffers, "new " + std::to_string(now.bytes.size()) + " bytes, the earlier patch has " + std::to_string(prior.bytes.size())};
        }
        for (std::size_t i = 0; i < now.bytes.size(); ++i) {
            if (now.bytes[i] != prior.bytes[i]) {
                return Keep{NarrowReason::BytesDiffer, "byte " + std::to_string(i) + " differs from the earlier patch"};
            }
        }
        if (now.relocs.size() != prior.relocs.size()) {
            return Keep{NarrowReason::TargetChanged, "the earlier patch has " + std::to_string(prior.relocs.size()) + " relocations, now " + std::to_string(now.relocs.size())};
        }
        for (std::size_t i = 0; i < now.relocs.size(); ++i) {
            const CodeReloc& a = now.relocs[i];
            const CodeReloc& b = prior.relocs[i];
            if (a == b) {
                continue;
            }
            if (a.target.kind == CodeTarget::Kind::NewSymbol) {
                return Keep{NarrowReason::TargetNew, "reference to the new symbol '" + a.target.name + "'"};
            }
            return Keep{NarrowReason::TargetChanged, "the relocation at byte " + std::to_string(a.offset) + " reaches another target than in the earlier patch"};
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

    // The extent of a read-only constant the patch carries as a fresh copy (string literal, constant table).
    std::optional<Keep> fresh_extent(const bind::SymbolDecision& d, const coff::Section** tsec_out, std::uint64_t* len_out) const {
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
        *tsec_out = tsec;
        *len_out = len;
        return std::nullopt;
    }

    // A constant that holds pointers (vtables, jump tables, function tables) is not judged: where its relocations
    // point cannot be compared, so the function that uses it is kept.
    std::optional<Keep> fresh_no_pointers(const bind::SymbolDecision& d, const coff::Section& tsec, std::uint64_t len) const {
        Result<std::vector<coff::ResolvedRelocation>> relocs = in_.object->resolve_relocations(tsec);
        if (!relocs) {
            return Keep{NarrowReason::Unverifiable, relocs.error().message};
        }
        for (const coff::ResolvedRelocation& rr : *relocs) {
            if (rr.reloc.virtual_address >= d.value && rr.reloc.virtual_address < d.value + len) {
                return Keep{NarrowReason::Unverifiable, "the constant '" + d.display + "' holds pointers"};
            }
        }
        return std::nullopt;
    }

    // A read-only constant the patch carries as a fresh copy: equal iff the bytes at the old target are equal.
    std::optional<Keep> check_fresh_constant(const bind::SymbolDecision& d, std::uint64_t old_target) const {
        const coff::Section* tsec = nullptr;
        std::uint64_t len = 0;
        if (std::optional<Keep> keep = fresh_extent(d, &tsec, &len)) {
            return keep;
        }
        if (old_target < in_.load_base || old_target - in_.load_base > 0xFFFFFFFFull) {
            return Keep{NarrowReason::TargetChanged, "'" + d.display + "' was reached outside the module in the old code"};
        }
        Result<pe::ByteSpan> old_const = in_.old_image->read_rva(static_cast<std::uint32_t>(old_target - in_.load_base), len);
        if (!old_const) {
            return Keep{NarrowReason::TargetChanged, "the old constant for '" + d.display + "' is not readable"};
        }
        if (std::optional<Keep> keep = fresh_no_pointers(d, *tsec, len)) {
            return keep;
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

bool finalize_code(CodeImage& image, const std::function<std::optional<std::uint64_t>(const std::string&)>& address_of) {
    for (CodeReloc& r : image.relocs) {
        if (r.target.kind != CodeTarget::Kind::NewSymbol) {
            continue;
        }
        const std::optional<std::uint64_t> va = address_of(r.target.name);
        if (!va) {
            return false;
        }
        r.target.kind = CodeTarget::Kind::Address;
        r.target.value = *va;
        r.target.name.clear();
    }
    return true;
}

std::optional<CodeImage> capture_code(const NarrowInput& input, const std::string& symbol, std::string* why) {
    if (input.plan == nullptr || input.object == nullptr) {
        return std::nullopt;
    }
    const Narrower narrower(input);
    CodeImage image;
    if (const std::optional<Keep> keep = narrower.capture_code(symbol, &image)) {
        if (why != nullptr) {
            *why = keep->detail;
        }
        return std::nullopt;
    }
    return image;
}

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
