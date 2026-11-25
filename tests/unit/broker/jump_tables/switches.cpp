// WP-49 (R-46): switch statements that tests/unit/broker/test_jump_tables.cpp compares with hook narrowing. Compiled by
// objects.cmake with the cell's compiler at /Od and at /O2 /Ob0 (own flags: no /RTC, no /GS, no default library).
//
// JT_SWAPPED exchanges the bodies of `case 1` and `case 2` of jt_dense: the code stays in the same order with the same bytes,
// only two entries of the jump table point elsewhere. Every case computes something else from `a`, so that no compiler turns
// the switch into a lookup table of constants.
extern "C" int jt_dense(int k, int a)
{
    int r = 0;
    switch (k) {
    case 0: r = a + 3; break;
#ifndef JT_SWAPPED
    case 1: r = a * 5; break;
    case 2: r = a - 7; break;
#else
    case 2: r = a * 5; break;
    case 1: r = a - 7; break;
#endif
    case 3: r = a ^ 11; break;
    case 4: r = a << 2; break;
    case 5: r = a * a; break;
    case 6: r = a | 64; break;
    default: r = -a; break;
    }
    return r;
}

// Sparse cases: cl may index a byte table first and then the jump table (a two-level switch).
extern "C" int jt_sparse(int k, int a)
{
    switch (k) {
    case 1: return a + 1;
    case 2: return a * 3;
    case 3: return a - 5;
    case 5: return a ^ 9;
    case 8: return a << 3;
    case 13: return a * a;
    case 21: return a | 32;
    case 34: return a & 15;
    case 55: return a >> 1;
    case 60: return ~a;
    default: return -a;
    }
}
