#pragma once

// Tiny logging / fatal-error helpers for the WP-03 spike (throwaway code).

#include <cstddef>
#include <cstdint>
#include <string>

namespace spike {

// printf-style; prints "[spike] <message>" to stdout and flushes (CI logs are the evidence).
void log(const char* fmt, ...);
// Prints "[spike] FATAL: <message>" to stdout and exits with code 1.
[[noreturn]] void die(const char* fmt, ...);

// "0x00007ff612340000"
std::string hex64(std::uint64_t v);
// "e9 00 11 ..." for the first `n` bytes.
std::string hex_bytes(const std::uint8_t* p, std::size_t n);

}  // namespace spike
