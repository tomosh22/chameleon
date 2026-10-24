#pragma once

#include <cstdint>
#include <string>

namespace cml {

// Every user-visible failure is a Diag: a stable numeric id (printed as CMLnnnn), a one-line
// message naming the symbol/file/function involved, and a "what to do" sentence.
//
// Id ranges:
//   CML1xxx parse / PDB      CML2xxx compile       CML3xxx link / bind
//   CML4xxx load / hook      CML5xxx runtime
enum class DiagCategory : std::uint8_t {
    Unknown = 0,
    Parse = 1,
    Compile = 2,
    Link = 3,
    Load = 4,
    Runtime = 5,
};

struct Diag {
    static constexpr std::uint32_t kMinCode = 1000;
    static constexpr std::uint32_t kMaxCode = 5999;

    std::uint32_t code = 0;  // e.g. 1101 for CML1101; 0 means "no diagnostic"
    std::string message;     // one line, names the symbol / file / function
    std::string what_to_do;  // one sentence the user can act on

    static Diag make(std::uint32_t code, std::string message, std::string what_to_do);

    // True when `code` lies inside one of the documented CML1xxx..CML5xxx ranges.
    bool valid() const noexcept;
    DiagCategory category() const noexcept;

    // "CML1101"; "CML0000" when the code is 0.
    std::string id() const;
    // "CML1101: <message> What to do: <what_to_do>"
    std::string format() const;
};

const char* to_string(DiagCategory category) noexcept;

}  // namespace cml
