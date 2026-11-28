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

// WP-52: `before` is the output up to the last reload, `after` the output since. The series of `key` in `after` has at
// least `min_after` values and its first is not below the last value of `before` (the reload kept the state, it did not
// restart it). Returns a one-line summary ("nest: 812 before the reload, 813 after, 40 samples since") or CML5427.
cml::Result<std::string> check_continues(std::string_view before, std::string_view after, std::string_view key, std::size_t min_after);

// WP-57: an expect_monotonic step with since_reload waits (up to within_s) until this is true: `output` (the whole output) holds
// at least `min_samples` values of `key` and its part from `reload_cursor` on (since the last reload) at least `since_reload`.
// Only then are check_monotonic and check_continues judged, so a fixture that has printed few lines by the time the step runs
// (a scenario soon after its launch) is given the window instead of failing at once.
bool monotonic_samples_ready(std::string_view output, std::size_t reload_cursor, std::string_view key, std::size_t min_samples, std::size_t since_reload);

}  // namespace cmltest
