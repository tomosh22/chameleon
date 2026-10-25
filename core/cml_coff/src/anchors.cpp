#include "cml/coff/anchors.h"

#include <cstdio>
#include <utility>

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
        case NoLocatorReason::HasLocator: return "has a locator";
        case NoLocatorReason::NotLinked: return "the section is not linked into the image";
        case NoLocatorReason::Associative: return "associative COMDAT sections have no locator (they never need retargeting to old data)";
        case NoLocatorReason::NoLeader: return "the COMDAT section has no symbol after its definition (located by an anchor)";
        case NoLocatorReason::StaticLeader: return "the COMDAT leader is a static symbol (located by an anchor)";
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
                      "Locate the section through its COMDAT parent or the PDB section contributions instead.");
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

    std::string tag = options.tag;
    if (tag.empty()) {
        auto bytes = write(m);  // for a parsed, unmodified object these are the compiler's file bytes
        if (!bytes) {
            return bytes.error();
        }
        tag = content_tag(ByteSpan(bytes->data(), bytes->size()));
    }
    if (options.used_tags != nullptr && !options.used_tags->insert(tag).second) {
        return Diag::make(diag::kAnchorTagCollision, "anchor tag '" + tag + "' is already used by another object of this link",
                          "Give every object of a link its own tag (for example its compiland index); identical objects cannot be linked twice.");
    }

    // Per section: position of the definition symbol and of the leader, both as indices into
    // m.symbols, plus raw indices; same rule as Object::comdat().
    const std::size_t nsec = m.sections.size();
    const std::size_t rec = m.symbol_record_size();
    constexpr std::int64_t kNone = -1;
    std::vector<std::int64_t> def_pos(nsec + 1, kNone);
    std::vector<std::int64_t> leader_pos(nsec + 1, kNone);
    std::vector<std::uint32_t> leader_raw(nsec + 1, 0);
    std::uint64_t raw = 0;
    for (std::size_t pos = 0; pos < m.symbols.size(); ++pos) {
        const ModelSymbol& s = m.symbols[pos];
        if (s.section_number > 0 && static_cast<std::size_t>(s.section_number) <= nsec) {
            const std::size_t sec = static_cast<std::size_t>(s.section_number);
            const bool is_def = s.storage_class == static_cast<std::uint8_t>(StorageClass::Static) && s.aux.size() >= rec &&
                                s.value == 0 && ((s.type >> 4) & 0xFu) != 2u;
            if (def_pos[sec] == kNone) {
                if (is_def) {
                    def_pos[sec] = static_cast<std::int64_t>(pos);
                }
            } else if (leader_pos[sec] == kNone && !is_def) {
                leader_pos[sec] = static_cast<std::int64_t>(pos);
                leader_raw[sec] = static_cast<std::uint32_t>(raw);
            }
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
        if (!sec.is_comdat()) {
            loc.kind = LocatorKind::Anchor;
            loc.reason = NoLocatorReason::HasLocator;
            ++wanted;
            continue;
        }
        const std::int64_t def = def_pos[i + 1];
        if (def != kNone) {
            const ModelSymbol& d = m.symbols[static_cast<std::size_t>(def)];
            if (d.aux.size() > 14 && std::to_integer<std::uint8_t>(d.aux[14]) == kSelectionAssociative) {
                loc.reason = NoLocatorReason::Associative;
                continue;
            }
        }
        const std::int64_t lp = leader_pos[i + 1];
        if (lp == kNone) {
            // No external symbol to find it by: an external anchor. Safe even in a COMDAT: the linkers
            // fold COMDATs across objects by the external leader name, so a leaderless COMDAT is never
            // discarded as a duplicate (and patches link with /OPT:NOREF).
            loc.kind = LocatorKind::Anchor;
            loc.reason = NoLocatorReason::NoLeader;
            ++wanted;
            continue;
        }
        const ModelSymbol& leader = m.symbols[static_cast<std::size_t>(lp)];
        if (leader.storage_class != static_cast<std::uint8_t>(StorageClass::External) || leader.name.empty()) {
            loc.kind = LocatorKind::Anchor;  // static leader: same reasoning as above
            loc.reason = NoLocatorReason::StaticLeader;
            ++wanted;
            continue;
        }
        loc.kind = LocatorKind::ComdatLeader;
        loc.reason = NoLocatorReason::HasLocator;
        loc.symbol_name = leader.name;
        loc.symbol_index = leader_raw[i + 1];
        loc.offset = leader.value;
    }

    if (raw + wanted > 0xFFFFFFFFull) {
        return Diag::make(diag::kSizeOverflow, "adding anchors would push the symbol table past 2^32 records",
                          "Split the module into smaller objects.");
    }
    m.symbols.reserve(m.symbols.size() + static_cast<std::size_t>(wanted));
    for (SectionLocator& loc : out) {
        if (loc.kind != LocatorKind::Anchor) {
            continue;
        }
        std::string name = anchor_name(loc.section_index, tag);
        auto encoded = m.encode_symbol_name(name);
        if (!encoded) {
            return encoded.error();
        }
        ModelSymbol sym;
        sym.name = name;
        sym.raw_name = *encoded;
        sym.value = 0;
        sym.section_number = static_cast<std::int32_t>(loc.section_index);
        sym.type = 0;
        sym.storage_class = static_cast<std::uint8_t>(StorageClass::External);
        m.symbols.push_back(std::move(sym));
        loc.symbol_name = std::move(name);
        loc.symbol_index = static_cast<std::uint32_t>(raw);
        loc.offset = 0;
        ++raw;
    }
    m.has_symbol_table = true;
    m.has_string_table = true;  // never leave a symbol table without its string-table size field
    CML_LOG(Trace, "coff: located %zu sections (%llu new anchors, tag %s)", nsec, static_cast<unsigned long long>(wanted), tag.c_str());
    return out;
}

}  // namespace cml::coff
