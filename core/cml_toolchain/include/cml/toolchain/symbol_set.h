#pragma once

// The symbol set of an object file, used to check that a recompiled translation unit defines and
// references the same symbols as the original compiland (WP-05 acceptance; WP-07/08 reuse it to
// sanity-check patch objects).

#include <set>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/util/result.h"

namespace cml::toolchain {

struct ObjectSymbolSet {
    std::set<std::string> defined_external;    // external symbols defined in a section or absolute
    std::set<std::string> undefined_external;  // external symbols referenced, not defined (includes common-less undefined)
    std::set<std::string> weak_external;       // IMAGE_SYM_CLASS_WEAK_EXTERNAL
    // Static (file-local) symbols defined in a section, minus compiler-generated labels: names
    // starting with '.' (section symbols) or '$' (cl's $LN/$SG/$pdata/$unwind labels).
    std::set<std::string> defined_static;

    std::size_t size() const noexcept {
        return defined_external.size() + undefined_external.size() + weak_external.size() + defined_static.size();
    }
};

ObjectSymbolSet symbol_set_of(const coff::Object& object);
// Reads and parses the file (CML12xx on failure).
Result<ObjectSymbolSet> symbol_set_of_file(const std::string& path);

struct SymbolSetDiff {
    // Lines like "defined_external: ?f@@YAHXZ" for names present only on one side.
    std::vector<std::string> only_in_first;
    std::vector<std::string> only_in_second;
    bool empty() const noexcept { return only_in_first.empty() && only_in_second.empty(); }
    std::string to_string() const;
};

SymbolSetDiff diff_symbol_sets(const ObjectSymbolSet& first, const ObjectSymbolSet& second);

}  // namespace cml::toolchain
