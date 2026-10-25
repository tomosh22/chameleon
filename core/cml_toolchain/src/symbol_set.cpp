#include "cml/toolchain/symbol_set.h"

#include <algorithm>

namespace cml::toolchain {

ObjectSymbolSet symbol_set_of(const coff::Object& object) {
    ObjectSymbolSet out;
    for (const coff::Symbol& s : object.symbols()) {
        const std::string name(s.name);
        if (name.empty()) {
            continue;
        }
        if (s.is_weak_external()) {
            out.weak_external.insert(name);
        } else if (s.is_external()) {
            if (s.section_number > 0 || s.is_absolute() || s.is_common()) {
                out.defined_external.insert(name);
            } else if (s.section_number == coff::kSectionUndefined) {
                out.undefined_external.insert(name);
            }
        } else if (s.is_static() && s.section_number > 0) {
            if (name[0] != '.' && name[0] != '$') {
                out.defined_static.insert(name);
            }
        }
    }
    return out;
}

Result<ObjectSymbolSet> symbol_set_of_file(const std::string& path) {
    Result<coff::OwnedObject> obj = coff::load_object_file(path);
    if (!obj) {
        return obj.error();
    }
    return symbol_set_of(obj->object);
}

namespace {

void diff_one(const char* kind, const std::set<std::string>& a, const std::set<std::string>& b, std::vector<std::string>& only_a,
              std::vector<std::string>& only_b) {
    for (const std::string& n : a) {
        if (b.find(n) == b.end()) {
            only_a.push_back(std::string(kind) + ": " + n);
        }
    }
    for (const std::string& n : b) {
        if (a.find(n) == a.end()) {
            only_b.push_back(std::string(kind) + ": " + n);
        }
    }
}

}  // namespace

SymbolSetDiff diff_symbol_sets(const ObjectSymbolSet& a, const ObjectSymbolSet& b) {
    SymbolSetDiff d;
    diff_one("defined_external", a.defined_external, b.defined_external, d.only_in_first, d.only_in_second);
    diff_one("undefined_external", a.undefined_external, b.undefined_external, d.only_in_first, d.only_in_second);
    diff_one("weak_external", a.weak_external, b.weak_external, d.only_in_first, d.only_in_second);
    diff_one("defined_static", a.defined_static, b.defined_static, d.only_in_first, d.only_in_second);
    return d;
}

std::string SymbolSetDiff::to_string() const {
    std::string out;
    for (const std::string& l : only_in_first) {
        out += "  only in first:  " + l + "\n";
    }
    for (const std::string& l : only_in_second) {
        out += "  only in second: " + l + "\n";
    }
    return out;
}

}  // namespace cml::toolchain
