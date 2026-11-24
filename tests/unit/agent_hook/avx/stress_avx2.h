#pragma once

// Functions of unit.agent_hook_stress that are compiled with /arch:AVX2 and optimised in every configuration
// (tests/unit/agent_hook/avx/CMakeLists.txt): leaf functions whose first instruction is VEX-encoded. WP-38, ADR-0042.
//
// Each family is int f(int x) returning x + k for 0 <= x < 2^23, computed in floating point so that the compiler has
// to use the AVX scalar instructions (vcvtsi2ss / vcvtsi2sd, preceded by a dependency-breaking vxorps on cl), and
// cannot fold the arithmetic back into an integer add.
namespace hooktest_avx2 {

int f_orig(int x);  // x + 1200   (float)
int f_a(int x);     // x + 2200
int f_b(int x);     // x + 3200
int d_orig(int x);  // x + 1300   (double)
int d_a(int x);     // x + 2300
int d_b(int x);     // x + 3300

}  // namespace hooktest_avx2
