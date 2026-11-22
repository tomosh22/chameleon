#pragma once

// Shared helpers of the fuzz targets (WP-28). Every target exports the libFuzzer entry point
//   extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t)
// and never aborts on malformed input: a reader that diagnoses (a Result<T> carrying a CMLnnnn id) is the
// expected outcome; a crash, a sanitizer report or an endless loop is the bug.

#include <cstddef>
#include <cstdint>

namespace cml::fuzz {

// A value the optimiser cannot prove unused, so the walks over parsed structures are not removed.
inline volatile std::uint64_t g_sink = 0;
inline void use(std::uint64_t v) noexcept { g_sink = g_sink + v; }

}  // namespace cml::fuzz

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);
