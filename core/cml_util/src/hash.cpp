#include "cml/util/hash.h"

// Header-only use of xxHash: every function is static inline in this translation unit.
#define XXH_INLINE_ALL
#include "xxhash.h"

namespace cml {

std::uint64_t hash64(const void* data, std::size_t size) noexcept {
    return static_cast<std::uint64_t>(XXH3_64bits(data, size));
}

}  // namespace cml
