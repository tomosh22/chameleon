#pragma once

// Small formatting helpers shared by the agent, the broker and the libraries (FU-10, WP-18). Header-only, no exceptions.

#include <cstdint>
#include <string>
#include <string_view>

namespace cml {

// "0x1f40": lower-case hexadecimal with a 0x prefix and no padding ("0x0" for zero). Used in diagnostics and log lines.
inline std::string hex(std::uint64_t value) {
    static const char kDigits[] = "0123456789abcdef";
    char buffer[2 + 16];
    char* end = buffer + sizeof(buffer);
    char* begin = end;
    do {
        *--begin = kDigits[value & 0xF];
        value >>= 4;
    } while (value != 0);
    *--begin = 'x';
    *--begin = '0';
    return std::string(begin, static_cast<std::size_t>(end - begin));
}

// ASCII upper-case copy of `s`.
inline std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

}  // namespace cml
