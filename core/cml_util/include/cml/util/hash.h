#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cml {

// 64-bit XXH3 of a byte range (unseeded). Stable across runs and machines.
std::uint64_t hash64(const void* data, std::size_t size) noexcept;

inline std::uint64_t hash64(std::string_view text) noexcept {
    return hash64(text.data(), text.size());
}

}  // namespace cml
