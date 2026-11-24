// See stress_avx2.h. Compiled with /arch:AVX2, optimised in every configuration. No header with inline functions is
// included: nothing compiled for AVX2 may be picked by the linker for code that runs on every CPU.
#include "stress_avx2.h"

#if defined(_MSC_VER)
#define HOOKTEST_AVX2_NOINLINE __declspec(noinline)
#else
#define HOOKTEST_AVX2_NOINLINE __attribute__((noinline))
#endif

namespace hooktest_avx2 {

// (float)x + k.5 is exact for 0 <= x < 2^23, and the conversion back truncates the half away: x + k. The half keeps
// a compiler from proving that the float arithmetic is an integer add.
HOOKTEST_AVX2_NOINLINE int f_orig(int x) { return static_cast<int>(static_cast<float>(x) + 1200.5f); }
HOOKTEST_AVX2_NOINLINE int f_a(int x) { return static_cast<int>(static_cast<float>(x) + 2200.5f); }
HOOKTEST_AVX2_NOINLINE int f_b(int x) { return static_cast<int>(static_cast<float>(x) + 3200.5f); }
HOOKTEST_AVX2_NOINLINE int d_orig(int x) { return static_cast<int>(static_cast<double>(x) + 1300.5); }
HOOKTEST_AVX2_NOINLINE int d_a(int x) { return static_cast<int>(static_cast<double>(x) + 2300.5); }
HOOKTEST_AVX2_NOINLINE int d_b(int x) { return static_cast<int>(static_cast<double>(x) + 3300.5); }

}  // namespace hooktest_avx2
