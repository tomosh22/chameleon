#pragma once

// Little-endian loads from byte pointers (x64 hosts, but memcpy keeps alignment/aliasing safe).

#include <cstdint>
#include <cstring>

namespace cml::pdb::detail {

inline std::uint16_t rd16(const std::uint8_t* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline std::uint32_t rd32(const std::uint8_t* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
inline std::uint64_t rd64(const std::uint8_t* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

}  // namespace cml::pdb::detail
