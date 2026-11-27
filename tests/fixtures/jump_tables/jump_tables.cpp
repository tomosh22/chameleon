#include "jump_tables.h"

// Two dense switches: cl compiles each to a jump table in the function's own code (Debug /Od and RelWithDebInfo /O2),
// clang-cl to a table in .rdata. Every case computes something else from `a`, so that no compiler turns a switch into a
// lookup table of constants.
__declspec(noinline) int jt_dense(int k, int a)
{
    int r = 0;
    switch (k) {
    case 0: r = a + 3; break;
    case 1: r = a * 5; break;
    case 2: r = a - 7; break;
    case 3: r = a ^ 11; break;
    case 4: r = a << 2; break;
    case 5: r = a * a; break;
    case 6: r = a | 64; break;
    default: r = -a; break;
    }
    return r;
}

__declspec(noinline) int jt_other(int k, int a)
{
    int r = 1;
    switch (k & 7) {
    case 0: r = a * 9; break;
    case 1: r = a + 100; break;
    case 2: r = a - 1; break;
    case 3: r = a ^ 0x55; break;
    case 4: r = a >> 1; break;
    case 5: r = a * 3 + 1; break;
    case 6: r = a & 6; break;
    default: r = a + a; break;
    }
    return r;
}

// No switch: the function the edits change.
__declspec(noinline) int jt_value()
{
    return 1;
}
