// See stress_avx2.h. Compiled with /arch:AVX2, optimised in every configuration. No header with inline functions is
// included: nothing compiled for AVX2 may be picked by the linker for code that runs on every CPU.
#include "stress_avx2.h"

#if defined(_MSC_VER)
#define HOOKTEST_AVX2_NOINLINE __declspec(noinline)
#else
#define HOOKTEST_AVX2_NOINLINE __attribute__((noinline))
#endif

namespace hooktest_avx2 {

// x + k.5 is exact in float for 0 <= x < 2^16 and k < 2^12, and truncating the result gives x + k.
HOOKTEST_AVX2_NOINLINE float f_orig(float x) { return x + 1200.5f; }
HOOKTEST_AVX2_NOINLINE float f_a(float x) { return x + 2200.5f; }
HOOKTEST_AVX2_NOINLINE float f_b(float x) { return x + 3200.5f; }
HOOKTEST_AVX2_NOINLINE double d_orig(double x) { return x + 1300.5; }
HOOKTEST_AVX2_NOINLINE double d_a(double x) { return x + 2300.5; }
HOOKTEST_AVX2_NOINLINE double d_b(double x) { return x + 3300.5; }

}  // namespace hooktest_avx2
