#include "cml/coff/anchors.h"

#include <cstdio>
#include <utility>

#include "cml/util/log.h"

namespace cml::coff {

namespace {

bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
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

std::string anchor_name(std::uint32_t section_index, std::string_view tag) {
    std::string name(kAnchorPrefix);
    if (!tag.empty()) {
        name.append(tag).push_back('_');
    }
    return name.append(std::to_string(section_index));
}

Result<std::vector<Anchor>> add_anchors(ObjectModel& m, const AnchorOptions& options) {
    if (options.external && options.tag.empty()) {
        return Diag::make(diag::kBadAnchorOptions, "external anchors need a tag (two objects of one link would define the same name)",
                          "Set AnchorOptions::tag to a string that is unique per object.");
    }
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
    std::uint64_t next_index = m.raw_symbol_count();
    std::vector<Anchor> anchors;
    std::size_t wanted = 0;
    for (const ModelSection& s : m.sections) {
        wanted += is_anchorable(s) ? 1u : 0u;
    }
    if (next_index + wanted > 0xFFFFFFFFull) {
        return Diag::make(diag::kSizeOverflow, "adding anchors would push the symbol table past 2^32 records",
                          "Split the module into smaller objects.");
    }
    anchors.reserve(wanted);
    m.symbols.reserve(m.symbols.size() + wanted);
    for (std::size_t i = 0; i < m.sections.size(); ++i) {
        if (!is_anchorable(m.sections[i])) {
            continue;
        }
        const std::uint32_t section_index = static_cast<std::uint32_t>(i + 1);
        std::string name = anchor_name(section_index, options.tag);
        auto raw = m.encode_symbol_name(name);
        if (!raw) {
            return raw.error();
        }
        ModelSymbol sym;
        sym.name = name;
        sym.raw_name = *raw;
        sym.value = 0;
        sym.section_number = static_cast<std::int32_t>(section_index);
        sym.type = 0;
        const bool external = options.external && !(options.comdat_static && m.sections[i].is_comdat());
        sym.storage_class = static_cast<std::uint8_t>(external ? StorageClass::External : StorageClass::Static);
        m.symbols.push_back(std::move(sym));
        anchors.push_back({section_index, static_cast<std::uint32_t>(next_index), std::move(name)});
        ++next_index;
    }
    m.has_symbol_table = true;
    m.has_string_table = true;  // never leave a symbol table without its string-table size field
    CML_LOG(Trace, "coff: added %zu anchors to %zu sections (symbol records now %llu)", anchors.size(), m.sections.size(),
            static_cast<unsigned long long>(next_index));
    return anchors;
}

}  // namespace cml::coff
