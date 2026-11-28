#include "cml/core/hook_narrowing.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_names.h"
#include "cml/pdb/type_records.h"
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

// Bytes from `value` to the next symbol defined in the same section (or the section end).
std::uint64_t section_extent(const coff::Object& object, const coff::Section& sec, std::uint32_t sec_index, std::uint32_t value) {
    std::uint64_t end = sec.data.size();
    for (const coff::Symbol& s : object.symbols()) {
        if (s.section_number != static_cast<std::int32_t>(sec_index) || s.value <= value) {
            continue;
        }
        end = std::min<std::uint64_t>(end, s.value);
    }
    return end > value ? end - value : 0;
}

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
            if (f.kind == bind::FixupKind::Addr32NbDisp) {
                imgrel_.insert((static_cast<std::uint64_t>(f.section_index) << 32) | f.offset);
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
        last_prior_.reset();
        if (const std::optional<Keep> keep = check(h)) {
            out.reason = keep->reason;
            out.detail = keep->detail;
        } else {
            out.reason = NarrowReason::Unchanged;
        }
        out.from_prior = last_prior_ != nullptr;
        return out;
    }

    // The record the last judge() compared with (null: it compared with the module file, or with nothing).
    std::shared_ptr<const CodeImage> last_prior() const { return last_prior_; }

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

    // ---- WP-49 (R-46): jump tables in the function's own code ---------------------------------------------------------

    // The ADDR32NB references of a function to places inside the function itself.
    struct LocalTargets {
        std::map<std::uint64_t, std::uint64_t> at;     // site (offset in the section) -> target offset from the function start
        std::map<std::uint64_t, std::string> refused;  // site -> why that reference cannot be judged (the function is hooked)
        std::size_t tables = 0;
        std::size_t entries = 0;
    };

    // Sorts the ADDR32NB sites of the function [start, start + len) of section `sec_index` that reach a place inside the
    // function into `at` (a jump-table entry, or a reference to the first entry of a table) and `refused` (everything else
    // that names a place of this section: a table whose entries leave the function, one whose end cannot be told, a
    // reference to a place that is not a table). The caller judges each site where it meets it, in relocation order, so a
    // function keeps the reason of its first relocation that differs. ADDR32NB sites the planner retargets to old data
    // (WP-36) and ADDR32NB to other sections are in neither map: the other rules judge them.
    void local_targets(const std::string& display, const coff::Section& sec, std::uint32_t sec_index, std::uint64_t start, std::uint64_t len,
                       const std::vector<coff::ResolvedRelocation>& relocs, LocalTargets* out) const {
        struct Ref {
            std::uint64_t target = 0;  // offset in the section the reference reaches (symbol value + in-place addend)
            std::uint64_t label = 0;   // the value of the symbol it names
            std::string name;
        };
        std::map<std::uint64_t, Ref> refs;  // site -> reference, ADDR32NB to a place inside the function
        bool addr32nb_at_end = false;       // an ADDR32NB right behind the function (a table may go on there)
        const std::uint64_t end = start + len;
        const auto refuse = [&](std::uint64_t site, std::string why) {
            CML_LOG(Trace, "WP49-JUMPTABLE function='%s' byte=%llu verdict=unverifiable reason=%s", display.c_str(), static_cast<unsigned long long>(site - start),
                    why.c_str());
            out->refused.emplace(site, std::move(why));
        };
        for (const coff::ResolvedRelocation& r : relocs) {
            if (r.reloc.type != coff::RelocType::Addr32Nb) {
                continue;
            }
            const std::uint64_t site = r.reloc.virtual_address;
            addr32nb_at_end = addr32nb_at_end || site == end;
            if (site < start || site >= end || site + 4 > sec.data.size() || r.target == nullptr) {
                continue;  // outside the function (a relocation crossing its bounds is refused by the caller)
            }
            if (planned_imgrel(sec_index, static_cast<std::uint32_t>(site)) || r.target->section_number != static_cast<std::int32_t>(sec_index)) {
                continue;  // old data (WP-36) or another section (a table in .rdata, a thunk): judged by the other rules
            }
            const std::uint32_t addend = static_cast<std::uint32_t>(load_le(sec.data.data() + site, 4));
            const std::uint64_t target = static_cast<std::uint32_t>(r.target->value + addend);
            if (target < start || target >= end) {
                refuse(site, "ADDR32NB to '" + std::string(r.target->name) + "' reaches +" + hex(target) +
                                 " of the section, outside the function (a jump table that leaves the function)");
                continue;
            }
            refs.emplace(site, Ref{target, r.target->value, std::string(r.target->name)});
        }
        if (refs.empty()) {
            return;
        }
        // A table starts where a reference points (at its target, or at the symbol it names when it carries an offset) and an
        // entry lies; it is the run of consecutive 4-byte entries from there.
        std::map<std::uint64_t, std::uint64_t> tables;  // first entry -> one past the last
        std::map<std::uint64_t, std::uint64_t> table_of;  // entry -> first entry of its table
        // The label fallback covers a table load whose displacement carries an index bias (`$LNt[rdx+rax*4-4]` for a switch
        // whose first case is 1): the reference then names the table's label with a small negative, entry-aligned offset. Any
        // other offset from a label that starts a table is not taken for a table load (the function stays hooked).
        constexpr std::uint64_t kMaxIndexBias = 16 * 4;
        const auto table_at = [&](const Ref& ref) -> std::optional<std::uint64_t> {
            if (refs.count(ref.target) != 0) {
                return ref.target;
            }
            const bool biased = ref.label > ref.target && ref.label - ref.target <= kMaxIndexBias && (ref.label - ref.target) % 4 == 0;
            if (biased && refs.count(ref.label) != 0) {
                return ref.label;
            }
            return std::nullopt;
        };
        for (const auto& [site, ref] : refs) {
            const std::optional<std::uint64_t> first = table_at(ref);
            if (!first || tables.count(*first) != 0) {
                continue;
            }
            std::uint64_t q = *first;
            while (refs.count(q) != 0) {
                table_of.emplace(q, *first);
                q += 4;
            }
            tables.emplace(*first, q);
        }
        // A table that runs to the end of the function with another ADDR32NB right behind it may go on outside the bytes
        // compared: its end cannot be told.
        std::set<std::uint64_t> unbounded;
        for (const auto& [first, last] : tables) {
            if (last >= end && addr32nb_at_end) {
                unbounded.insert(first);
            }
        }
        for (const auto& [site, ref] : refs) {
            const auto entry = table_of.find(site);
            const std::optional<std::uint64_t> loads = table_at(ref);
            if (entry == table_of.end() && !loads) {
                refuse(site, "ADDR32NB to '" + ref.name + "' reaches byte " + std::to_string(ref.target - start) +
                                 " of the function, which is not the start of a jump table (no entries there)");
                continue;
            }
            const std::uint64_t table = entry != table_of.end() ? entry->second : *loads;
            if (unbounded.count(table) != 0 || (loads && unbounded.count(*loads) != 0)) {
                refuse(site, "the jump table at byte " + std::to_string(table - start) + " runs to the end of the function and an ADDR32NB follows it: its end cannot be told");
                continue;
            }
            out->at.emplace(site, ref.target - start);
        }
        out->tables = tables.size();
        out->entries = table_of.size();
        CML_LOG(Trace, "WP49-JUMPTABLE function='%s' tables=%zu entries=%zu references=%zu refused=%zu verdict=%s", display.c_str(), out->tables, out->entries,
                refs.size() - out->entries, out->refused.size(), out->refused.empty() ? "comparable" : "unverifiable");
    }

    // R-50 (as in the WP-36 prover): a relocation that starts before the function and reaches into it, or one of a type whose
    // width is not known less than 8 bytes before it, would leave function bytes unmasked or unexplained: not judged.
    static std::optional<Keep> reaches_into_start(const std::vector<coff::ResolvedRelocation>& relocs, std::uint64_t start) {
        for (const coff::ResolvedRelocation& r : relocs) {
            const std::uint64_t site = r.reloc.virtual_address;
            if (site >= start || start - site >= 8) {
                continue;
            }
            const std::uint32_t w = reloc_width(r.reloc.type);
            if (w == 0 || site + w > start) {
                return Keep{NarrowReason::Unverifiable, "the relocation " + std::string(coff::reloc_type_name(r.reloc.type)) + " " + std::to_string(start - site) +
                                                            " bytes before the function reaches into it"};
            }
        }
        return std::nullopt;
    }

    // nullopt: identical, drop the hook.
    std::optional<Keep> check(const bind::HookEntry& h) const {
        if (in_.object == nullptr) {
            return Keep{NarrowReason::Unverifiable, "no object to compare"};
        }
        const bool hooked = in_.is_hooked && in_.is_hooked(h.old_va);
        if (hooked || h.old_rva == bind::kNoRva) {
            // WP-25: the entry runs code of an earlier patch (hooked, or defined by one): compare with what the broker recorded.
            if (in_.prior_code) {
                if (std::shared_ptr<const CodeImage> prior = in_.prior_code(h.old_va)) {
                    last_prior_ = prior;
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
        std::string relocs_error;
        const std::vector<coff::ResolvedRelocation>* relocs = relocs_of(*sec, &relocs_error);
        if (relocs == nullptr) {
            return Keep{NarrowReason::Unverifiable, relocs_error};
        }
        // R-50: before the bytes, so that an unmasked relocation never reads as a spurious "bytes differ" (WP-40/WP-46 count that
        // as a changed function).
        if (std::optional<Keep> keep = reaches_into_start(*relocs, start)) {
            return keep;
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
        // (After the bytes, so that a function whose bytes differ is reported as changed, as before WP-49.)
        LocalTargets local;
        local_targets(h.display, *sec, dec->section, start, len, *relocs, &local);
        for (const coff::ResolvedRelocation* r : inside) {
            if (const auto no = local.refused.find(r->reloc.virtual_address); no != local.refused.end()) {
                return Keep{NarrowReason::Unverifiable, no->second};
            }
            if (const auto l = local.at.find(r->reloc.virtual_address); l != local.at.end()) {
                // WP-49: the old linker wrote the RVA of the place in the old function; the same place relative to the start.
                const std::uint64_t at = r->reloc.virtual_address - start;
                const std::uint32_t old_raw = static_cast<std::uint32_t>(load_le(old_bytes->data() + at, 4));
                const std::uint32_t old_off = old_raw - h.old_rva;
                if (old_off != l->second) {
                    CML_LOG(Trace, "WP49-JUMPTABLE function='%s' verdict=target-changed byte=%llu old=+%s new=+%s", h.display.c_str(), static_cast<unsigned long long>(at),
                            hex(old_off).c_str(), hex(l->second).c_str());
                    return Keep{NarrowReason::TargetChanged, "the jump-table reference at byte " + std::to_string(at) + " reached byte " + std::to_string(old_off) +
                                                                 " of the old function, now byte " + std::to_string(l->second)};
                }
                continue;
            }
            if (std::optional<Keep> keep = check_target(h, *sec, dec->section, start, *r, *old_bytes)) {
                return keep;
            }
        }
        if (local.tables != 0) {
            CML_LOG(Trace, "WP49-JUMPTABLE function='%s' tables=%zu entries=%zu verdict=equal", h.display.c_str(), local.tables, local.entries);
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
        std::string relocs_error;
        const std::vector<coff::ResolvedRelocation>* relocs = relocs_of(sec, &relocs_error);
        if (relocs == nullptr) {
            return Keep{NarrowReason::Unverifiable, relocs_error};
        }
        if (std::optional<Keep> keep = reaches_into_start(*relocs, loc.start)) {
            return keep;
        }
        LocalTargets local;  // WP-49
        local_targets(loc.dec->display.empty() ? symbol : loc.dec->display, sec, loc.dec->section, loc.start, loc.len, *relocs, &local);
        out->symbol = symbol;
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
            if (const auto no = local.refused.find(site); no != local.refused.end()) {
                return Keep{NarrowReason::Unverifiable, no->second};
            }
            if (const auto l = local.at.find(site); l != local.at.end()) {
                cr.target.kind = CodeTarget::Kind::Local;  // WP-49: the same place of the function, wherever it lies
                cr.target.value = l->second;
            } else if (std::optional<Keep> keep = capture_target(loc.dec->section, r, &cr.target)) {
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
        if (r.reloc.type == coff::RelocType::Addr32Nb) {
            // WP-36: an image-relative displacement the planner retargets to old data (an Addr32NbDisp fix-up) reaches a
            // fixed address, like a REL32 to bound data. Every other ADDR32NB in code (a jump-table load, a thunk)
            // depends on where the patch lies: not judged.
            if (!planned_imgrel(sec_index, r.reloc.virtual_address) || d.decision != bind::DecisionKind::BindOldData || !d.old_address.valid) {
                return Keep{NarrowReason::Unverifiable, "ADDR32NB to '" + name + "' in code is not a retargeted displacement"};
            }
            out->kind = CodeTarget::Kind::Address;
            out->value = d.old_address.va;
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
                if (d.decision == bind::DecisionKind::PatchHooked) {
                    out->local_callee = d.name;  // the linked patch calls its own copy of it
                }
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
        // WP-49 review: in a section that holds more than one function (no /Gy), the extent runs to the next symbol, and a label
        // inside the function (a case label, a jump table) cuts it short; the earlier patch's record was cut the same way, so
        // the bytes behind the label would never be compared. The PDB's size of the function checks the extent: without it, or
        // when it differs, the hook is kept. (A COMDAT function is its whole section; its size may differ from the module's
        // because an earlier patch changed it.)
        Loc loc;
        const bool located = !locate(h.new_symbol, &loc);
        if (!located || !loc.sec->is_comdat()) {
            if (h.old_size == 0) {
                return Keep{NarrowReason::UnknownOldSize, "the function shares its section with other symbols and the PDB gives no size to check its extent"};
            }
            if (now.bytes.size() != h.old_size) {
                return Keep{NarrowReason::SizeDiffers, "the function shares its section with other symbols; its extent there is " + std::to_string(now.bytes.size()) +
                                                           " bytes, the PDB's size " + std::to_string(h.old_size)};
            }
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
            if (a.target.kind == CodeTarget::Kind::Local && b.target.kind == CodeTarget::Kind::Local && a.offset == b.offset && a.type == b.type &&
                a.target.value == b.target.value) {
                continue;  // WP-49: the same place of the function, whether named by a label or by the section with an offset
            }
            if (a.target.kind == CodeTarget::Kind::NewSymbol) {
                return Keep{NarrowReason::TargetNew, "reference to the new symbol '" + a.target.name + "'"};
            }
            if (a.target.kind == CodeTarget::Kind::Local && b.target.kind == CodeTarget::Kind::Local && a.offset == b.offset && a.target.value != b.target.value) {
                CML_LOG(Trace, "WP49-JUMPTABLE function='%s' verdict=target-changed byte=%u prior=+%s new=+%s", h.display.c_str(), a.offset, hex(b.target.value).c_str(),
                        hex(a.target.value).c_str());
                return Keep{NarrowReason::TargetChanged, "the jump-table reference at byte " + std::to_string(a.offset) + " reached byte " + std::to_string(b.target.value) +
                                                             " of the earlier patch's function, now byte " + std::to_string(a.target.value)};
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
        } else if (r.reloc.type == coff::RelocType::Addr32Nb && planned_imgrel(sec_index, static_cast<std::uint32_t>(site)) &&
                   d.decision == bind::DecisionKind::BindOldData) {
            // WP-36: the old linker wrote RVA(S) + A into the displacement (32-bit arithmetic); the new code gets an
            // Addr32NbDisp fix-up to the bound address. Same target = same code. Other ADDR32NB sites stay unjudged.
            old_target = in_.load_base + static_cast<std::uint32_t>(static_cast<std::uint32_t>(old_raw) - static_cast<std::uint32_t>(new_raw));
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
        std::string relocs_error;
        const std::vector<coff::ResolvedRelocation>* relocs = relocs_of(tsec, &relocs_error);
        if (relocs == nullptr) {
            return Keep{NarrowReason::Unverifiable, relocs_error};
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
        return section_extent(*in_.object, sec, sec_index, value);
    }

    // The relocations of a section with their targets, resolved once per section.
    const std::vector<coff::ResolvedRelocation>* relocs_of(const coff::Section& sec, std::string* error) const {
        const auto it = relocs_.find(&sec);
        if (it != relocs_.end()) {
            return &it->second;
        }
        Result<std::vector<coff::ResolvedRelocation>> resolved = in_.object->resolve_relocations(sec);
        if (!resolved) {
            *error = resolved.error().message;
            return nullptr;
        }
        return &relocs_.emplace(&sec, std::move(*resolved)).first->second;
    }

    const bind::SymbolDecision* by_raw_name(const std::string& name) const {
        return in_.plan->find_decision(name);
    }

    const NarrowInput& in_;
    std::unordered_map<std::uint32_t, const bind::SymbolDecision*> by_raw_index_;
    std::unordered_map<std::uint64_t, const bind::Fixup*> secrel_;
    std::set<std::uint64_t> imgrel_;  // WP-36: (section << 32 | offset) of the planned Addr32NbDisp fix-ups
    bool planned_imgrel(std::uint32_t sec_index, std::uint32_t offset) const {
        return imgrel_.count((static_cast<std::uint64_t>(sec_index) << 32) | offset) != 0;
    }
    mutable std::unordered_map<const coff::Section*, std::vector<coff::ResolvedRelocation>> relocs_;
    mutable std::shared_ptr<const CodeImage> last_prior_;
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

struct CodeRecorder::Impl {
    explicit Impl(const NarrowInput& in) : narrower(in) {}
    Narrower narrower;
};

CodeRecorder::CodeRecorder(const NarrowInput& input) {
    if (input.plan != nullptr && input.object != nullptr) {
        impl_ = std::make_unique<Impl>(input);
    }
}

CodeRecorder::~CodeRecorder() = default;

std::optional<CodeImage> CodeRecorder::capture(const std::string& symbol, std::string* why) const {
    if (!impl_) {
        return std::nullopt;
    }
    CodeImage image;
    if (const std::optional<Keep> keep = impl_->narrower.capture_code(symbol, &image)) {
        if (why != nullptr) {
            *why = keep->detail;
        }
        return std::nullopt;
    }
    return image;
}

std::optional<CodeImage> capture_code(const NarrowInput& input, const std::string& symbol, std::string* why) {
    const CodeRecorder recorder(input);
    return recorder.capture(symbol, why);
}

NarrowResult narrow_hooks(const NarrowInput& input) {
    NarrowResult out;
    if (input.plan == nullptr) {
        return out;
    }
    const Narrower narrower(input);
    const std::vector<bind::HookEntry>& planned = input.plan->hooks;
    out.decisions.reserve(planned.size());
    std::vector<std::shared_ptr<const CodeImage>> priors;
    priors.reserve(planned.size());
    std::unordered_map<std::string, std::size_t> by_symbol;
    for (std::size_t i = 0; i < planned.size(); ++i) {
        out.decisions.push_back(narrower.judge(planned[i]));
        priors.push_back(narrower.last_prior());
        by_symbol.emplace(planned[i].new_symbol, i);
    }
    // A function judged equal to the code an earlier patch put at its entry still calls THAT patch's copy of every function the
    // patch defines and hooks. If such a callee is hooked again now, the earlier copy of the caller would go on calling the stale
    // copy of the callee: the caller is hooked again as well (repeat: this can ripple up a chain of callers).
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < planned.size(); ++i) {
            NarrowDecision& d = out.decisions[i];
            if (!is_dropped(d.reason) || !priors[i]) {
                continue;
            }
            for (const CodeReloc& r : priors[i]->relocs) {
                if (r.target.local_callee.empty()) {
                    continue;
                }
                const auto callee = by_symbol.find(r.target.local_callee);
                if (callee != by_symbol.end() && !is_dropped(out.decisions[callee->second].reason)) {
                    d.reason = NarrowReason::TargetChanged;
                    d.detail = "it calls '" + planned[callee->second].display + "', which is hooked again; the earlier patch's copy of this function calls the earlier copy";
                    changed = true;
                    break;
                }
            }
        }
    }
    std::set<std::uint64_t> all_entries;
    std::set<std::uint64_t> kept_entries;
    for (std::size_t i = 0; i < planned.size(); ++i) {
        const NarrowDecision& d = out.decisions[i];
        CML_LOG(Trace, "narrow: %s -> %s%s%s", planned[i].display.c_str(), to_string(d.reason), d.detail.empty() ? "" : ": ", d.detail.c_str());
        all_entries.insert(planned[i].old_va);
        if (!is_dropped(d.reason) && kept_entries.insert(planned[i].old_va).second) {
            out.hooks.push_back(planned[i]);
        }
    }
    out.planned = all_entries.size();
    out.dropped = all_entries.size() - kept_entries.size();
    return out;
}

std::vector<std::string> bound_data_changes(const NarrowInput& input, std::size_t limit) {
    std::vector<std::string> changed;
    if (input.plan == nullptr || input.object == nullptr || input.old_image == nullptr) {
        return changed;
    }
    for (const bind::SymbolDecision& d : input.plan->decisions) {
        if (changed.size() >= limit) {
            break;
        }
        if (d.decision != bind::DecisionKind::BindOldData || !d.defined || d.section == 0 || d.type != bind::SymbolType::Data) {
            continue;
        }
        const coff::Section* sec = input.object->section(d.section);
        if (sec == nullptr || sec->is_code() || sec->data.empty() || !d.old_address.valid || d.old_address.rva == bind::kNoRva) {
            continue;  // uninitialised, or nothing to compare with
        }
        const std::uint64_t len = sec->is_comdat() ? sec->data.size() - std::min<std::uint64_t>(d.value, sec->data.size())
                                                   : section_extent(*input.object, *sec, d.section, d.value);
        if (len == 0 || len > kMaxConstantBytes || d.value + len > sec->data.size()) {
            continue;
        }
        Result<pe::ByteSpan> old_bytes = input.old_image->read_rva(d.old_address.rva, len);
        Result<std::vector<coff::ResolvedRelocation>> relocs = input.object->resolve_relocations(*sec);
        if (!old_bytes || !relocs) {
            continue;
        }
        bool holds_pointers = false;
        for (const coff::ResolvedRelocation& rr : *relocs) {
            holds_pointers = holds_pointers || (rr.reloc.virtual_address >= d.value && rr.reloc.virtual_address < d.value + len);
        }
        if (holds_pointers) {
            continue;
        }
        for (std::uint64_t i = 0; i < len; ++i) {
            if (sec->data[d.value + i] != (*old_bytes)[i]) {
                changed.push_back(d.display);
                break;
            }
        }
    }
    return changed;
}


// ---- WP-40 (R-48 item 5, ADR-0043 rule (d)): lambdas told apart by the code of their call operators ----------------------

namespace {

enum class Equivalence : std::uint8_t { Yes, No, Unknown };

// Replaces the closure `from` of a decorated name (after its "??R") by `to`.
std::string with_closure(const std::string& name, const std::string& from, const std::string& to) {
    const std::size_t at = name.find("??R" + from);
    if (at == std::string::npos) {
        return {};
    }
    return name.substr(0, at + 3) + to + name.substr(at + 3 + from.size());
}

}  // namespace

LambdaPairing pair_lambdas_by_code(const NarrowInput& input) {
    LambdaPairing out;
    if (input.plan == nullptr || input.object == nullptr) {
        return out;
    }
    const bind::BindPlan& plan = *input.plan;
    for (const bind::LambdaSet& set : plan.lambda_sets) {
        const std::size_t n = set.lambdas.size();
        // The hooks of the call operators as planned (paired by number): the old entry of lambda j is hooks[j].
        std::vector<const bind::HookEntry*> hook(n, nullptr);
        for (std::size_t i = 0; i < n; ++i) {
            for (const bind::HookEntry& h : plan.hooks) {
                if (h.new_symbol == set.lambdas[i].call_operator) {
                    hook[i] = &h;
                }
            }
        }
        std::vector<std::vector<Equivalence>> eq(n, std::vector<Equivalence>(n, Equivalence::Unknown));
        std::string unknown_why;
        bind::BindPlan hypo = plan;  // a copy whose decisions are changed per hypothesis
        // The closures are named without the translation unit's discriminator ("?A0x<hash>@" of an anonymous namespace):
        // the decorated names are compared in that spelling (WP-40 review).
        std::map<std::string, const bind::SymbolDecision*> by_plain_name;
        for (const bind::SymbolDecision& d : plan.decisions) {
            if (d.defined && d.type == bind::SymbolType::Data) {
                by_plain_name.emplace(pdb::without_tu_discriminator(d.name), &d);
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                const bind::LambdaEntry& a = set.lambdas[i];
                const bind::LambdaEntry& b = set.lambdas[j];
                if (hook[j] == nullptr) {
                    unknown_why = "the call operator '" + b.call_operator + "' is not hooked to an old one";
                    continue;
                }
                // Hypothesis: the patch's lambda i is the old lambda j. Its statics then live where the plan put lambda j's
                // (which it paired with old lambda j by number), and its call operator is compared with old lambda j's code.
                hypo.decisions = plan.decisions;
                bool same_statics = true;
                // Every datum declared in lambda i (its statics and their guards, also the guards the planner placed by the
                // section layout, which the closure step did not pair).
                std::vector<std::pair<std::string, std::string>> statics_of_a;  // (decorated, plain)
                for (const auto& [plain, d] : by_plain_name) {
                    if (plain.find("??R" + a.closure) != std::string::npos) {
                        statics_of_a.emplace_back(d->name, plain);
                    }
                }
                for (const auto& [st, plain] : statics_of_a) {
                    const std::string twin = i == j ? plain : with_closure(plain, a.closure, b.closure);
                    const auto found = by_plain_name.find(twin);
                    const bind::SymbolDecision* dj = found == by_plain_name.end() ? nullptr : found->second;
                    if (dj == nullptr) {
                        same_statics = false;
                        break;
                    }
                    for (bind::SymbolDecision& d : hypo.decisions) {
                        if (d.name == st) {
                            d.decision = dj->decision;
                            d.source = dj->source;
                            d.old_address = dj->old_address;
                        }
                    }
                }
                if (!same_statics) {
                    eq[i][j] = Equivalence::No;
                    CML_LOG(Trace, "lambdas of '%s': %s vs old %s: not the same statics", set.function.c_str(), a.closure.c_str(), b.old_closure.c_str());
                    continue;
                }
                bind::HookEntry h = *hook[j];
                h.new_symbol = a.call_operator;
                hypo.hooks.assign(1, h);
                NarrowInput ni = input;
                ni.plan = &hypo;
                ni.enabled = true;
                const Narrower narrower(ni);
                const NarrowDecision d = narrower.judge(h);
                switch (d.reason) {
                    case NarrowReason::Unchanged: eq[i][j] = Equivalence::Yes; break;
                    case NarrowReason::BytesDiffer:
                    case NarrowReason::SizeDiffers:
                    case NarrowReason::TargetChanged:
                    case NarrowReason::TargetNew: eq[i][j] = Equivalence::No; break;
                    default:
                        eq[i][j] = Equivalence::Unknown;
                        unknown_why = "the code of '" + bind::display_name(a.call_operator) + "' cannot be compared with the old lambda's: " + to_string(d.reason) +
                                      (d.detail.empty() ? std::string() : " - " + d.detail);
                        break;
                }
                CML_LOG(Trace, "lambdas of '%s': %s vs old %s: %s%s%s", set.function.c_str(), a.closure.c_str(), b.old_closure.c_str(), to_string(d.reason),
                        d.detail.empty() ? "" : " - ", d.detail.c_str());
            }
        }
        // Rule 1: one-to-one equivalence. Rule 2: one changed lambda left on each side. Rule 3: anything else by number.
        std::vector<std::size_t> pair(n, n);
        std::vector<bool> old_taken(n, false);
        bool unknown = !unknown_why.empty();
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < n; ++j) {
                unknown = unknown || eq[i][j] == Equivalence::Unknown;
            }
        }
        if (!unknown) {
            for (std::size_t i = 0; i < n; ++i) {
                std::size_t row = 0;
                std::size_t hit = n;
                for (std::size_t j = 0; j < n; ++j) {
                    if (eq[i][j] == Equivalence::Yes) {
                        ++row;
                        hit = j;
                    }
                }
                std::size_t column = 0;
                for (std::size_t k = 0; hit != n && k < n; ++k) {
                    column += eq[k][hit] == Equivalence::Yes ? 1u : 0u;
                }
                if (row == 1 && column == 1) {
                    pair[i] = hit;
                    old_taken[hit] = true;
                }
            }
        }
        std::vector<std::size_t> new_left;
        std::vector<std::size_t> old_left;
        for (std::size_t i = 0; i < n; ++i) {
            if (pair[i] == n) {
                new_left.push_back(i);
            }
            if (!old_taken[i]) {
                old_left.push_back(i);
            }
        }
        bool by_number = false;
        std::vector<std::size_t> by_number_lambdas;  // the new lambdas paired by their number (rule 3)
        std::vector<std::size_t> taken_number;       // the new lambdas whose number's old lambda another one took (refused)
        if (unknown) {
            by_number = true;
            for (std::size_t i = 0; i < n; ++i) {
                pair[i] = i;
                by_number_lambdas.push_back(i);
            }
        } else if (new_left.size() == 1 && old_left.size() == 1) {
            pair[new_left[0]] = old_left[0];  // rule 2: the one lambda whose body changed
        } else if (!new_left.empty()) {
            // Rule 3 (the orchestrator's ruling): what is left pairs by its NUMBER. Where the old lambda of that number was
            // already taken by a code-equivalent new one, nothing can be said about this one: refused.
            by_number = true;
            for (const std::size_t i : new_left) {
                if (!old_taken[i]) {
                    pair[i] = i;
                    old_taken[i] = true;
                    by_number_lambdas.push_back(i);
                } else {
                    taken_number.push_back(i);
                }
            }
        }
        const auto names_of = [&](const std::vector<std::size_t>& which) {
            std::string text;
            for (const std::size_t i : which) {
                text += (text.empty() ? "" : ", ") + set.lambdas[i].closure.substr(0, set.lambdas[i].closure.find('@'));
            }
            return text;
        };
        if (!taken_number.empty()) {
            out.errors.push_back(Diag::make(bind::diag::kLambdasUnpairable,
                                            "Lambdas " + names_of(taken_number) + " of '" + set.function +
                                                "' cannot be matched to the running program's lambdas: their code changed, and the old lambda of their number is another new lambda's (by the code of its call operator), so the values of their statics would go to the wrong lambda",
                                            "Keep the order of these lambdas while you edit them, rename their statics, or hot-restart."));
            CML_LOG(Info, "WP40-LAMBDAS function='%s' lambdas=%zu refused=%zu", set.function.c_str(), n, taken_number.size());
            continue;
        }
        std::size_t moved = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (pair[i] != i && pair[i] < n) {
                out.closure_pairing[set.lambdas[i].closure] = set.lambdas[pair[i]].old_closure;
                ++moved;
                CML_LOG(Trace, "lambdas of '%s': %s is the old %s (by the code of its call operator)", set.function.c_str(), set.lambdas[i].closure.c_str(),
                        set.lambdas[pair[i]].old_closure.c_str());
            }
        }
        if (by_number && !by_number_lambdas.empty()) {
            out.warnings.push_back(Diag::make(bind::diag::kLambdasMatchedByNumber,
                                              "Lambdas " + names_of(by_number_lambdas) + " of '" + set.function + "' are matched by their numbers" +
                                                  (unknown ? " (" + unknown_why + ")" : std::string(" (their code changed, so it cannot tell them apart)")) +
                                                  "; if you reordered them, the values of their statics may be exchanged.",
                                              "Rename the statics, keep the order of these lambdas, or hot-restart after reordering."));
        }
        CML_LOG(Info, "WP40-LAMBDAS function='%s' lambdas=%zu moved=%zu by_number=%d", set.function.c_str(), n, moved, by_number ? 1 : 0);
    }
    return out;
}

std::vector<Diag> prove_hashed_lambdas(const NarrowInput& input) {
    std::vector<Diag> out;
    if (input.plan == nullptr || input.plan->hashed_lambdas.empty()) {
        return out;
    }
    const bind::BindPlan& plan = *input.plan;
    NarrowInput ni = input;
    ni.enabled = true;
    const Narrower narrower(ni);
    std::map<std::string, const bind::HookEntry*> hook_of;
    for (const bind::HookEntry& h : plan.hooks) {
        hook_of.emplace(h.new_symbol, &h);
    }
    std::vector<std::string> changed;   // closures whose code changed
    std::vector<std::string> unknown;   // closures whose code cannot be compared
    std::string unknown_why;
    std::size_t unchanged = 0;
    std::size_t eliminated = 0;  // paired by elimination: the changed lambda (its code is not the old one's, by construction)
    for (const bind::HashedLambda& lambda : plan.hashed_lambdas) {
        if (!lambda.old_closure.empty()) {
            ++eliminated;
            bool hooked = !lambda.functions.empty();
            for (const std::string& function : lambda.functions) {
                hooked = hooked && hook_of.count(function) != 0;
            }
            CML_LOG(Trace, "hashed lambda %s: paired by elimination with the old %s%s", lambda.closure.c_str(), lambda.old_closure.c_str(), hooked ? "" : " (a function is not hooked)");
            if (!hooked) {
                unknown.push_back(lambda.closure);
                unknown_why = unknown_why.empty() ? "the call operator of '" + lambda.closure + "', paired by elimination with '" + lambda.old_closure + "', is not hooked to the old one" : unknown_why;
            }
            continue;
        }
        Equivalence verdict = Equivalence::Yes;
        std::string why;
        for (const std::string& function : lambda.functions) {
            const auto hook = hook_of.find(function);
            if (hook == hook_of.end()) {
                verdict = Equivalence::Unknown;
                why = "'" + bind::display_name(function) + "' is not hooked to an old function";
                break;
            }
            const NarrowDecision d = narrower.judge(*hook->second);
            CML_LOG(Trace, "hashed lambda %s: %s -> %s%s%s", lambda.closure.c_str(), bind::display_name(function).c_str(), to_string(d.reason), d.detail.empty() ? "" : ": ",
                    d.detail.c_str());
            switch (d.reason) {
                case NarrowReason::Unchanged: break;
                case NarrowReason::BytesDiffer:
                case NarrowReason::SizeDiffers:
                case NarrowReason::TargetChanged:
                case NarrowReason::TargetNew: verdict = verdict == Equivalence::Unknown ? verdict : Equivalence::No; break;
                default:
                    verdict = Equivalence::Unknown;
                    why = "the code of '" + bind::display_name(function) + "' cannot be compared with the old code: " + to_string(d.reason) + (d.detail.empty() ? std::string() : " - " + d.detail);
                    break;
            }
            if (verdict == Equivalence::Unknown) {
                break;
            }
        }
        if (verdict == Equivalence::Yes) {
            ++unchanged;
        } else if (verdict == Equivalence::No) {
            changed.push_back(lambda.closure);
        } else {
            unknown.push_back(lambda.closure);
            unknown_why = unknown_why.empty() ? why : unknown_why;
        }
    }
    // ADR-0043 rule (d), as ruled for these lambdas (WP-46, D1): every lambda whose code is the old one's is that lambda; one
    // lambda left whose code changed is the one the edit changed. Two or more changed, or one that cannot be compared: the
    // names are all that pairs them, and nothing proves them.
    // (The lambda paired by elimination is the one changed lambda rule (d) allows; then every name-matched one must be unchanged.)
    const bool proven = unknown.empty() && changed.size() + eliminated <= 1;
    CML_LOG(Info, "WP46-HASHED-LAMBDAS object=%s closures=%zu unchanged=%zu changed=%zu eliminated=%zu unknown=%zu proven=%s", plan.object_name.c_str(), plan.hashed_lambdas.size(),
            unchanged, changed.size(), eliminated, unknown.size(),
            proven ? (eliminated != 0 ? "by-elimination" : (changed.empty() ? "unchanged" : "one-changed")) : "refused");
    if (proven) {
        return out;
    }
    const std::vector<std::string>& named = unknown.empty() ? changed : unknown;
    std::string list;
    for (std::size_t i = 0; i < named.size() && i < 8; ++i) {
        list += (list.empty() ? "" : ", ") + named[i];
    }
    if (named.size() > 8) {
        list += " and " + std::to_string(named.size() - 8) + " more";
    }
    const std::string reason =
        !unknown.empty() ? unknown_why
        : eliminated != 0 ? "one lambda was paired by elimination (its name changed with its code) while " + std::to_string(changed.size()) +
                                " lambda(s) whose names were kept changed as well, so their code cannot say which old lambda each one is"
                          : std::to_string(changed.size()) + " of them changed, so their code cannot say which old lambda each one is";
    out.push_back(Diag::make(bind::diag::kHashedLambdasUnproven,
                             "Lambdas " + list + " in " + plan.object_name +
                                 " are named by a hash (cl's legacy lambda processor) and were paired with the old lambdas of the same name, which only their code can confirm, and " + reason +
                                 "; a wrong pairing would run one lambda's code, or give its statics, for another",
                             "Change at most one lambda of this file per reload, compile with /Zc:lambda (the default under /std:c++20), or hot-restart."));
    return out;
}

std::vector<Diag> prove_bitmask_guards(const NarrowInput& input) {
    std::vector<Diag> out;
    if (input.plan == nullptr) {
        return out;
    }
    const bind::BindPlan& plan = *input.plan;
    for (const bind::BitmaskGuard& bg : plan.bitmask_guards) {
        std::string why;
        const bind::HookEntry* hook = nullptr;
        for (const bind::HookEntry& h : plan.hooks) {
            if (!bg.function.empty() && h.new_symbol == bg.function) {
                hook = &h;
                break;
            }
        }
        if (bg.function.empty()) {
            why = "the function it belongs to cannot be read from its name";
        } else if (hook == nullptr) {
            why = "'" + bind::display_name(bg.function) + "' is not the old version of a function of the running program (new, or not found)";
        } else if (input.object == nullptr || input.old_image == nullptr) {
            why = "the old code of '" + bind::display_name(bg.function) + "' is not available";
        } else {
            NarrowInput ni = input;
            ni.enabled = true;
            const Narrower narrower(ni);
            const NarrowDecision d = narrower.judge(*hook);
            if (d.reason == NarrowReason::Unchanged) {
                CML_LOG(Info, "WP40-BITMASK guard=%s function='%s' proven=unchanged%s", bg.guard.c_str(), bind::display_name(bg.function).c_str(), d.from_prior ? " (against an earlier patch)" : "");
                continue;
            }
            why = "the code of '" + bind::display_name(bg.function) + "' changed (" + to_string(d.reason) + (d.detail.empty() ? std::string() : ": " + d.detail) + ")";
        }
        CML_LOG(Info, "WP40-BITMASK guard=%s function='%s' proven=refused", bg.guard.c_str(), bg.function.empty() ? "?" : bind::display_name(bg.function).c_str());
        out.push_back(Diag::make(bind::diag::kGuardMismatch,
                                 "the bitmask guard '" + bg.guard + "' (no thread-safe statics) cannot be bound to the running program's: its bits go to the function's dynamically initialised statics in the order they are reached, which only an unchanged function proves to be as before, and " +
                                     why + "; with the old bitmask a static could be constructed again or never",
                                 "Leave this function unchanged while the program runs (edit other functions of the file), or hot-restart."));
    }
    return out;
}

std::vector<Diag> prove_inlined_lambda_statics(const NarrowInput& input) {
    std::vector<Diag> out;
    if (input.plan == nullptr || input.plan->inlined_lambda_statics.empty()) {
        return out;
    }
    const bind::BindPlan& plan = *input.plan;
    // One verdict per procedure: "" proven (unchanged), else why not.
    std::map<std::uint32_t, std::string> verdicts;
    const auto verdict_of = [&](const bind::InlinedLambdaStatic& s) -> const std::string& {
        const auto found = verdicts.find(s.proc_rva);
        if (found != verdicts.end()) {
            return found->second;
        }
        std::string why;
        const bind::HookEntry* hook = nullptr;
        for (const bind::HookEntry& h : plan.hooks) {
            if (s.proc_rva != bind::kNoRva && h.old_rva == s.proc_rva) {
                hook = &h;
                break;
            }
        }
        if (hook == nullptr) {
            why = "'" + s.proc + "' is not paired with a function of the patch (its old entry at rva " + hex(s.proc_rva) + " is not hooked)";
        } else if (input.object == nullptr || input.old_image == nullptr) {
            why = "the old code of '" + s.proc + "' is not available";
        } else {
            NarrowInput ni = input;
            ni.enabled = true;
            const Narrower narrower(ni);
            const NarrowDecision d = narrower.judge(*hook);
            if (d.reason != NarrowReason::Unchanged) {
                why = "the code of '" + s.proc + "' changed (" + std::string(to_string(d.reason)) + (d.detail.empty() ? std::string() : ": " + d.detail) + ")";
            } else {
                CML_LOG(Trace, "WP53-INLINED-PROC proc='%s' rva=%s unchanged%s", s.proc.c_str(), hex(s.proc_rva).c_str(), d.from_prior ? " (against an earlier patch)" : "");
            }
        }
        return verdicts.emplace(s.proc_rva, std::move(why)).first->second;
    };
    std::size_t proven = 0;
    std::size_t refused = 0;
    for (const bind::InlinedLambdaStatic& s : plan.inlined_lambda_statics) {
        const bind::SymbolDecision* d = plan.find_decision(s.symbol);
        if (d == nullptr || d->decision != bind::DecisionKind::BindOldData || d->old_address.rva != s.record_rva) {
            CML_LOG(Trace, "WP53-INLINED-PROOF static='%s' proc='%s' skipped (no longer bound to the record at rva %s)", s.display.c_str(), s.proc.c_str(), hex(s.record_rva).c_str());
            continue;
        }
        const std::string& why = verdict_of(s);
        CML_LOG(Trace, "WP53-INLINED-PROOF static='%s' record rva=%s proc='%s' by=%s proven=%s%s%s", s.display.c_str(), hex(s.record_rva).c_str(), s.proc.c_str(), bind::to_string(s.how),
                why.empty() ? "unchanged" : "refused", why.empty() ? "" : " ", why.c_str());
        if (why.empty()) {
            ++proven;
            continue;
        }
        ++refused;
        out.push_back(Diag::make(bind::diag::kAmbiguousStatic,
                                 "static data '" + s.display + "' (" + s.symbol + ") in " + plan.object_name + " was paired with the record at rva " + hex(s.record_rva) + " of '" + s.proc +
                                     "', which inlined its lambda; a compiler records the statics of an inlined lambda there and the function's own statics alike, so only an unchanged '" +
                                     s.proc + "' proves the record is this lambda's (it may be a static the edit deleted from '" + s.proc + "'), and " + why,
                                 "Leave '" + s.proc + "' unchanged while the program runs (edit other functions of the file), rename the static, or hot-restart."));
    }
    CML_LOG(Info, "WP53-INLINED object=%s listed=%zu proven=%zu refused=%zu", plan.object_name.c_str(), plan.inlined_lambda_statics.size(), proven, refused);
    return out;
}

}  // namespace cml::core
