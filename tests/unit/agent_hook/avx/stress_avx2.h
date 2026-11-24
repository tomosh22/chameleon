#pragma once

// Functions of unit.agent_hook_stress that are compiled with /arch:AVX2 and optimised in every configuration
// (tests/unit/agent_hook/avx/CMakeLists.txt): leaf functions whose first instruction is VEX-encoded. WP-38, ADR-0042.
//
// Each adds a constant to its argument in floating point. The argument arrives in xmm0, so the first thing an
// optimising compiler can do with it is the addition itself: vaddss / vaddsd xmm0, xmm0, [rip + constant]
// (C5 FA 58 05 .. / C5 FB 58 05 ..), a VEX instruction with a RIP-relative operand at the entry. (With an integer
// argument cl first stores ecx to its home slot, which is not VEX.) The stress test calls them with small
// non-negative integers and truncates the result, so f(x) counts as x + k.
namespace hooktest_avx2 {

float f_orig(float x);    // x + 1200.5
float f_a(float x);       // x + 2200.5
float f_b(float x);       // x + 3200.5
double d_orig(double x);  // x + 1300.5
double d_a(double x);     // x + 2300.5
double d_b(double x);     // x + 3300.5

}  // namespace hooktest_avx2
