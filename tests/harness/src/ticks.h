#pragma once

// Reading `key=<integer>` out of the fixtures' `TICK k=v ...` lines (WP-12). Used by the `expect_monotonic` step:
// a counter that lives in the old image's storage (the `calls=` of fixture `statics`) must keep counting across a
// reload; if the patch had its own copy, it would restart from its initial value.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "cml/util/result.h"

namespace cmltest {

// The values of `key=` on every line that starts with "TICK ", in order. Lines without the key, or with a value
// that is not an integer, are skipped. The key matches whole words only ("calls" does not match "recalls=").
std::vector<long long> tick_values(std::string_view output, std::string_view key);

// Checks that the series of `key` never decreases, increases at least once, and has at least `min_samples`
// values. Returns a one-line summary ("calls: 143 samples, 1 -> 143") or CML5427 naming the first offence.
cml::Result<std::string> check_monotonic(std::string_view output, std::string_view key, std::size_t min_samples);

}  // namespace cmltest
