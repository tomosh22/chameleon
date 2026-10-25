#pragma once

// Name -> item index. A sorted array of (hash32 << 32 | item index): 8 bytes per item, O(n log n) to
// build and O(log n + matches) to query, with no clustering however many items share a name (a
// module can hold thousands of function-local statics called "calls").

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "cml/util/hash.h"
#include "detail/counting.h"

namespace cml::pdb::detail {

inline std::uint32_t name_hash32(std::string_view name) noexcept {
    return static_cast<std::uint32_t>(cml::hash64(name.data(), name.size()));
}

class NameIndex {
public:
    // `name_of(i)` returns the name of item i, for i in [0, count).
    template <class NameOf>
    void build(std::size_t count, NameOf&& name_of) {
        keys_.clear();
        keys_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            keys_.push_back((static_cast<std::uint64_t>(name_hash32(name_of(i))) << 32) | static_cast<std::uint64_t>(i));
        }
        std::sort(keys_.begin(), keys_.end());
    }

    // Calls f(index) for every item whose name equals `name`, in ascending index order.
    template <class NameOf, class F>
    void find(std::string_view name, NameOf&& name_of, F&& f) const {
        const std::uint64_t h = static_cast<std::uint64_t>(name_hash32(name)) << 32;
        auto it = std::lower_bound(keys_.begin(), keys_.end(), h);
        for (; it != keys_.end() && (*it & 0xFFFFFFFF00000000ull) == h; ++it) {
            const std::size_t idx = static_cast<std::size_t>(*it & 0xFFFFFFFFull);
            if (name_of(idx) == name) {
                f(idx);
            }
        }
    }

    std::size_t size() const noexcept { return keys_.size(); }

private:
    Vec<std::uint64_t> keys_;
};

}  // namespace cml::pdb::detail
