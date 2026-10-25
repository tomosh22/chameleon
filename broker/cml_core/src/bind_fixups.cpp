#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <set>

#include "cml/core/bind_diag_ids.h"
#include "cml/core/bind_plan.h"
#include "cml/util/log.h"

namespace cml::core::bind {

namespace {
std::string hex(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}
}  // namespace

Result<std::vector<FixupTriple>> make_fixup_triples(const BindPlan& plan, const PatchLayout& layout) {
    std::vector<FixupTriple> out;
    out.reserve(plan.fixups.size());
    for (const Fixup& f : plan.fixups) {
        FixupTriple t;
        t.kind = f.kind;
        if (f.kind == FixupKind::Slot) {
            const auto it = layout.symbol_rva.find(f.symbol);
            if (it == layout.symbol_rva.end()) {
                return Diag::make(diag::kLayoutIncomplete, "the patch layout has no RVA for the slot symbol '" + f.symbol + "'",
                                  "Look the symbol up as a public in the patch PDB (it is defined by binding.obj).");
            }
            t.rva = it->second;
            t.value = f.target_va;
        } else {
            const auto it = layout.section_rva.find(f.section_index);
            if (it == layout.section_rva.end()) {
                return Diag::make(diag::kNoLocator,
                                  "the patch layout has no RVA for object section " + std::to_string(f.section_index) + " ('" + f.section_name +
                                      "'), which holds a reference to '" + f.symbol + "'",
                                  "Locate the section through its anchor or COMDAT leader in the patch PDB.");
            }
            t.rva = it->second + f.offset;
            const std::uint64_t site = layout.patch_base + t.rva;
            const std::uint64_t target = f.target_va + static_cast<std::uint64_t>(f.addend);  // S + A (wraps like the machine)
            if (is_rel32(f.kind)) {
                const std::int64_t disp = static_cast<std::int64_t>(target - (site + 4u + rel32_extra(f.kind)));
                if (disp < INT32_MIN || disp > INT32_MAX) {
                    return Diag::make(diag::kRel32OutOfRange,
                                      "the " + std::string(to_string(f.kind)) + " displacement to '" + f.symbol + "' does not fit 32 bits: site " + hex(site) +
                                          ", target " + hex(target) + " (section '" + f.section_name + "' + " + hex(f.offset) + ")",
                                      "Reserve the patch within +-2 GiB of the module (the loader does this), or hot-restart.");
                }
                t.value = static_cast<std::uint32_t>(static_cast<std::int32_t>(disp));
            } else {
                t.value = target;
            }
        }
        out.push_back(t);
    }
    std::stable_sort(out.begin(), out.end(), [](const FixupTriple& a, const FixupTriple& b) { return a.rva < b.rva; });
    // The same site twice (e.g. a COMDAT reached through two relocations of one section) is fine when
    // both writes agree; conflicting writes are a planner bug.
    std::vector<FixupTriple> unique;
    unique.reserve(out.size());
    for (const FixupTriple& t : out) {
        if (!unique.empty() && unique.back().rva == t.rva) {
            if (unique.back().kind == t.kind && unique.back().value == t.value) {
                continue;
            }
            return Diag::make(diag::kLayoutIncomplete, "two different fix-ups write the same site at rva " + hex(t.rva),
                              "This is a bug in the bind planner; report it with the plan JSON.");
        }
        unique.push_back(t);
    }
    return unique;
}

Result<PatchLayout> layout_from_pdb(const BindPlan& plan, std::span<const coff::SectionLocator> locators, const pdb::PdbIndex& patch_pdb,
                                    std::uint64_t patch_base) {
    PatchLayout layout;
    layout.patch_base = patch_base;
    std::set<std::uint32_t> sections;
    for (const Fixup& f : plan.fixups) {
        if (f.kind != FixupKind::Slot) {
            sections.insert(f.section_index);
        }
    }
    for (const std::uint32_t sec : sections) {
        if (sec == 0 || sec > locators.size()) {
            return Diag::make(diag::kNoLocator, "object section " + std::to_string(sec) + " has no locator entry", "Pass the locators add_anchors() returned for this object.");
        }
        const coff::SectionLocator& loc = locators[sec - 1];
        if (loc.kind == coff::LocatorKind::None) {
            return Diag::make(diag::kNoLocator,
                              "object section " + std::to_string(sec) + " needs a fix-up but has no locator (" +
                                  (loc.reason == coff::NoLocatorReason::Associative ? "associative COMDAT" : "not linked") + ")",
                              "Move the reference out of unwind or debug data, or hot-restart.");
        }
        Result<std::vector<pdb::SymbolView>> pubs = patch_pdb.find_public(loc.symbol_name);
        if (!pubs) {
            return pubs.error();
        }
        if (pubs->empty() || pubs->front().rva == pdb::kNoRva) {
            return Diag::make(diag::kLayoutIncomplete,
                              "locator symbol '" + loc.symbol_name + "' of object section " + std::to_string(sec) + " is not a public of the patch PDB",
                              "Link the patch with /OPT:NOREF /OPT:NOICF /DEBUG:FULL (WP-09 does this).");
        }
        layout.section_rva[sec] = pubs->front().rva - loc.offset;
    }
    for (const Fixup& f : plan.fixups) {
        if (f.kind != FixupKind::Slot || layout.symbol_rva.count(f.symbol) != 0) {
            continue;
        }
        Result<std::vector<pdb::SymbolView>> pubs = patch_pdb.find_public(f.symbol);
        if (!pubs) {
            return pubs.error();
        }
        if (pubs->empty() || pubs->front().rva == pdb::kNoRva) {
            return Diag::make(diag::kLayoutIncomplete, "slot symbol '" + f.symbol + "' is not a public of the patch PDB",
                              "binding.obj must be linked into the patch with its symbols external.");
        }
        layout.symbol_rva[f.symbol] = pubs->front().rva;
    }
    CML_LOG(Trace, "bind: layout from patch PDB: %zu sections, %zu slot symbols", layout.section_rva.size(), layout.symbol_rva.size());
    return layout;
}

}  // namespace cml::core::bind
