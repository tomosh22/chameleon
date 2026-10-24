#include "cml/util/diag.h"

#include <cstdio>
#include <utility>

namespace cml {

Diag Diag::make(std::uint32_t code, std::string message, std::string what_to_do) {
    Diag d;
    d.code = code;
    d.message = std::move(message);
    d.what_to_do = std::move(what_to_do);
    return d;
}

bool Diag::valid() const noexcept {
    return code >= kMinCode && code <= kMaxCode;
}

DiagCategory Diag::category() const noexcept {
    if (!valid()) {
        return DiagCategory::Unknown;
    }
    return static_cast<DiagCategory>(code / 1000u);
}

std::string Diag::id() const {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "CML%04u", static_cast<unsigned>(code % 10000u));
    return std::string(buf);
}

std::string Diag::format() const {
    std::string out = id();
    out += ": ";
    out += message;
    if (!what_to_do.empty()) {
        out += " What to do: ";
        out += what_to_do;
    }
    return out;
}

const char* to_string(DiagCategory category) noexcept {
    switch (category) {
        case DiagCategory::Parse:
            return "parse";
        case DiagCategory::Compile:
            return "compile";
        case DiagCategory::Link:
            return "link";
        case DiagCategory::Load:
            return "load";
        case DiagCategory::Runtime:
            return "runtime";
        case DiagCategory::Unknown:
            break;
    }
    return "unknown";
}

}  // namespace cml
