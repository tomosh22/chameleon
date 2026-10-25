#pragma once

// The one place that defines which symbol is a section's "definition" symbol and which one is its
// COMDAT "leader". Used by the reader (Object::comdat) and by the writer side (add_anchors), so the
// rule cannot drift between them.
//
//   * A section-definition symbol is a STATIC symbol with value 0, defined in a section (> 0), that
//     has at least one aux record and is not a function (DTYPE_FUNCTION in the type's derived nibble).
//   * The leader of a section is the first symbol of that section that FOLLOWS its definition symbol
//     in the symbol table and is not itself a definition symbol. A symbol of the section that precedes
//     the definition symbol is ignored.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cml::coff {

inline constexpr std::uint8_t kStaticStorageClass = 3;  // IMAGE_SYM_CLASS_STATIC

inline constexpr bool is_section_definition_symbol(std::uint8_t storage_class, std::int32_t section_number,
                                                   std::size_t aux_count, std::uint32_t value, std::uint16_t type) noexcept {
    return storage_class == kStaticStorageClass && section_number > 0 && aux_count >= 1 && value == 0 &&
           ((type >> 4) & 0xFu) != 2u;
}

// Feed every PRIMARY symbol in table order; afterwards definition(s) / leader(s) give the position
// (the caller's own running position, e.g. the index among primary symbols) per 1-based section.
class SectionSymbolTracker {
public:
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

    explicit SectionSymbolTracker(std::size_t section_count)
        : def_(section_count + 1, kNone), leader_(section_count + 1, kNone) {}

    void add(std::uint32_t position, std::int32_t section_number, bool is_definition) {
        if (section_number <= 0 || static_cast<std::size_t>(section_number) >= def_.size()) {
            return;
        }
        const std::size_t sec = static_cast<std::size_t>(section_number);
        if (def_[sec] == kNone) {
            if (is_definition) {
                def_[sec] = position;
            }
            // A symbol of this section that precedes its definition symbol is not the leader.
        } else if (leader_[sec] == kNone && !is_definition) {
            leader_[sec] = position;
        }
    }

    std::uint32_t definition(std::size_t section) const noexcept { return section < def_.size() ? def_[section] : kNone; }
    std::uint32_t leader(std::size_t section) const noexcept { return section < leader_.size() ? leader_[section] : kNone; }

    const std::vector<std::uint32_t>& definitions() const noexcept { return def_; }
    const std::vector<std::uint32_t>& leaders() const noexcept { return leader_; }

private:
    std::vector<std::uint32_t> def_;
    std::vector<std::uint32_t> leader_;
};

}  // namespace cml::coff
