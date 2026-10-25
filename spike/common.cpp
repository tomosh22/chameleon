#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace spike {

namespace {
std::string vformat(const char* fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        return std::string("<format error>");
    }
    std::vector<char> buf(static_cast<std::size_t>(n) + 1);
    std::vsnprintf(buf.data(), buf.size(), fmt, ap);
    return std::string(buf.data(), static_cast<std::size_t>(n));
}
}  // namespace

void log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const std::string s = vformat(fmt, ap);
    va_end(ap);
    std::printf("[spike] %s\n", s.c_str());
    std::fflush(stdout);
}

void die(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const std::string s = vformat(fmt, ap);
    va_end(ap);
    std::printf("[spike] FATAL: %s\n", s.c_str());
    std::fflush(stdout);
    std::exit(1);
}

std::string hex64(std::uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string hex_bytes(const std::uint8_t* p, std::size_t n)
{
    std::string s;
    char buf[8];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof buf, "%02x", static_cast<unsigned>(p[i]));
        if (i != 0) {
            s += ' ';
        }
        s += buf;
    }
    return s;
}

}  // namespace spike
