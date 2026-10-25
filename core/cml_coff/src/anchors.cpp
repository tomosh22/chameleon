#include "cml/coff/anchors.h"

#include <cstdio>
#include <utility>

#include "cml/coff/section_symbols.h"
#include "cml/util/hash.h"
#include "cml/util/log.h"

namespace cml::coff {

namespace {

bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

constexpr std::uint8_t kSelectionAssociative = 5;

const char* reason_text(NoLocatorReason r) noexcept {
    switch (r) {
        case NoLocatorReason::NotLinked:
            return "the section is not linked into the image (.drectve, .debug*, LNK_REMOVE / LNK_INFO / discardable)";
        case NoLocatorReason::Associative:
            return "the section is an associative COMDAT child (.pdata/.xdata/...): it is placed with its parent section and has no locator of its own";
        case NoLocatorReason::HasLocator:
        case NoLocatorReason::NoLeader:
        case NoLocatorReason::StaticLeader:
            return "the section has a locator";
    }
    return "unknown";
}

}  // namespace

bool is_anchorable(const ModelSection& s) noexcept {
    if ((s.characteristics & (scn::LnkInfo | scn::LnkRemove | scn::MemDiscardable)) != 0) {
        return false;
    }
    if (starts_with(s.name, ".debug") || starts_with(s.name, ".llvm_")) {
        return false;
    }
    return true;
}

std::string content_tag(ByteSpan object_bytes) {
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx",
                  static_cast<unsigned long long>(hash64(object_bytes.data(), object_bytes.size())));
    return std::string(out);
}

Result<void> check_unique_tags(std::span<const std::string> tags) {
    std::unordered_set<std::string> seen;
    seen.reserve(tags.size());
    for (const std::string& t : tags) {
        if (!seen.insert(t).second) {
            return Diag::make(diag::kAnchorTagCollision, "anchor tag '" + t + "' is used by two objects of one link",
                              "Give every object of a link its own tag (for example its compiland index).");
        }
    }
    return {};
}

std::string anchor_name(std::uint32_t section_index, std::string_view tag) {
    std::string name(kAnchorPrefix);
    if (!tag.empty()) {
        name.append(tag).push_back('_');
    }
    return name.append(std::to_string(section_index));
}

Result<void> require_locator(const SectionLocator& l) {
    if (l.kind != LocatorKind::None) {
        return {};
    }
    return Diag::make(diag::kNoLocator,
                      "section " + std::to_string(l.section_index) + " has no locator: " + reason_text(l.reason),
                      "Locate the section through its parent section (associated section number) or the PDB section contributions.");
}

Result<std::vector<SectionLocator>> add_anchors(ObjectModel& m, const AnchorOptions& options) {
    for (char c : options.tag) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return Diag::make(diag::kBadAnchorOptions, "anchor tag '" + options.tag + "' contains a character other than letters, digits and '_'",
                              "Use only letters, digits and '_' in the tag.");
        }
    }
    for (const ModelSymbol& s : m.symbols) {
        if (starts_with(s.name, kAnchorPrefix)) {
            return Diag::make(diag::kAnchorNameClash,
                              "object already contains the symbol '" + s.name + "'; it looks anchored already",
                              "Anchor the compiler's original object, not an already anchored one.");
        }
    }

    // ---- analysis (read only): the leader rule is shared with the reader ----
    const std::size_t nsec = m.sections.size();
    const std::size_t rec = m.symbol_record_size();
    SectionSymbolTracker tracker(nsec);
    constexpr std::int64_t kNone = -1;
    std::vector<std::uint32_t> raw_of(m.symbols.size());
    std::vector<std::int64_t> first_external(nsec + 1, kNone);  // any external symbol defined in the section
    std::uint64_t raw = 0;
    for (std::size_t pos = 0; pos < m.symbols.size(); ++pos) {
        const ModelSymbol& s = m.symbols[pos];
        raw_of[pos] = static_cast<std::uint32_t>(raw);
        tracker.add(static_cast<std::uint32_t>(pos), s.section_number,
                    is_section_definition_symbol(s.storage_class, s.section_number, s.aux.size() / rec, s.value, s.type));
        if (s.section_number > 0 && static_cast<std::size_t>(s.section_number) <= nsec &&
            s.storage_class == static_cast<std::uint8_t>(StorageClass::External) && !s.name.empty() &&
            first_external[static_cast<std::size_t>(s.section_number)] == kNone) {
            first_external[static_cast<std::size_t>(s.section_number)] = static_cast<std::int64_t>(pos);
        }
        raw += 1 + s.aux.size() / rec;
    }

    std::uint64_t wanted = 0;
    std::vector<SectionLocator> out(nsec);
    for (std::size_t i = 0; i < nsec; ++i) {
        const ModelSection& sec = m.sections[i];
        SectionLocator& loc = out[i];
        loc.section_index = static_cast<std::uint32_t>(i + 1);
        if (!is_anchorable(sec)) {
            loc.reason = NoLocatorReason::NotLinked;
            continue;
        }
        const auto anchor = [&](NoLocatorReason why) {
            loc.kind = LocatorKind::Anchor;
            loc.reason = why;
            ++wanted;
        };
        if (!sec.is_comdat()) {
            anchor(NoLocatorReason::HasLocator);
            continue;
        }
        const std::uint32_t def = tracker.definition(i + 1);
        if (def != SectionSymbolTracker::kNone) {
            const ModelSymbol& d = m.symbols[def];
            if (d.aux.size() > 14 && std::to_integer<std::uint8_t>(d.aux[14]) == kSelectionAssociative) {
                loc.reason = NoLocatorReason::Associative;
                continue;
            }
        }
        // The leader, if external, is the locator. If the symbol after the definition is not external
        // (a static function), any other external symbol defined in the section serves; only when
        // there is none, an external anchor is added. That anchor is safe inside a COMDAT because
        // the linkers fold COMDATs across objects by the external leader name: a COMDAT without an
        // external symbol is never selected against another copy, and /OPT:REF is off in patch links.
        const std::uint32_t lp = tracker.leader(i + 1);
        std::int64_t use = kNone;
        if (lp != SectionSymbolTracker::kNone &&
            m.symbols[lp].storage_class == static_cast<std::uint8_t>(StorageClass::External) && !m.symbols[lp].name.empty()) {
            use = lp;
        } else if (first_external[i + 1] != kNone) {
            use = first_external[i + 1];
        }
        if (use == kNone) {
            anchor(lp == SectionSymbolTracker::kNone ? NoLocatorReason::NoLeader : NoLocatorReason::StaticLeader);
            continue;
        }
        const ModelSymbol& ext = m.symbols[static_cast<std::size_t>(use)];
        loc.kind = LocatorKind::ComdatLeader;
        loc.reason = NoLocatorReason::HasLocator;
        loc.symbol_name = ext.name;
        loc.symbol_index = raw_of[static_cast<std::size_t>(use)];
        loc.offset = ext.value;
    }

    if (wanted == 0) {
        return out;  // nothing to add: no tag, no used_tags entry, no change
    }
    if (raw + wanted > 0xFFFFFFFFull) {
        return Diag::make(diag::kSizeOverflow, "adding anchors would push the symbol table past 2^32 records",
                          "Split the module into smaller objects.");
    }

    // ---- tag ----
    std::string tag = options.tag;
    if (tag.empty()) {
        if (!options.object_bytes.empty()) {
            tag = content_tag(options.object_bytes);
        } else {
            auto bytes = write(m);  // for a parsed, unmodified object these are the compiler's file bytes
            if (!bytes) {
                return bytes.error();
            }
            tag = content_tag(ByteSpan(bytes->data(), bytes->size()));
        }
    }
    if (options.used_tags != nullptr && options.used_tags->count(tag) != 0) {
        return Diag::make(diag::kAnchorTagCollision, "anchor tag '" + tag + "' is already used by another object of this link",
                          "Give every object of a link its own tag (for example its compiland index); identical objects cannot be linked twice.");
    }

    // ---- append (transactional: undone completely on failure) ----
    const std::size_t old_symbols = m.symbols.size();
    const std::size_t old_strings = m.string_table.size();
    const bool old_has_symbol_table = m.has_symbol_table;
    const bool old_has_string_table = m.has_string_table;
    const auto rollback = [&] {
        m.symbols.resize(old_symbols);
        m.string_table.resize(old_strings);
        m.has_symbol_table = old_has_symbol_table;
        m.has_string_table = old_has_string_table;
    };
    m.symbols.reserve(old_symbols + static_cast<std::size_t>(wanted));
    std::uint64_t next = raw;
    for (SectionLocator& loc : out) {
        if (loc.kind != LocatorKind::Anchor) {
            continue;
        }
        std::string name = anchor_name(loc.section_index, tag);
        auto encoded = m.encode_symbol_name(name);
        if (!encoded) {
            rollback();
            return encoded.error();
        }
        ModelSymbol sym;
        sym.name = name;
        sym.raw_name = *encoded;
        sym.section_number = static_cast<std::int32_t>(loc.section_index);
        sym.storage_class = static_cast<std::uint8_t>(StorageClass::External);
        m.symbols.push_back(std::move(sym));
        loc.symbol_name = std::move(name);
        loc.symbol_index = static_cast<std::uint32_t>(next);
        loc.offset = 0;
        ++next;
    }
    m.has_symbol_table = true;
    m.has_string_table = true;  // never leave a symbol table without its string-table size field
    if (options.used_tags != nullptr) {
        options.used_tags->insert(tag);  // only now that everything succeeded
    }
    CML_LOG(Trace, "coff: located %zu sections (%llu new anchors, tag %s)", nsec, static_cast<unsigned long long>(wanted), tag.c_str());
    return out;
}

}  // namespace cml::coff
