/* Fixture `imgrel`: the translation unit the scenario edits. See imgrel.h. Keep the shape of both functions: it is
 * the one for which cl 19.4x /O2 emits `sym@IMGREL` displacements (ADR-0042, spike run 45402685481). */
#include "imgrel.h"

struct Core CORE;
static int sarr[64];
int (*fptab[8])(int);

int ig_mixed(int type, int p)
{
    switch (type) {
        case 0: CORE.keys[p] = 0; break;
        case 1:
            CORE.keys[p] = 1;
            if (CORE.prev[p] == 0 && CORE.count < 16) {
                CORE.q[CORE.count] = p;
                CORE.count++;
            }
            break;
        case 2: sarr[p & 63] = ig_g(p); break;
        case 3: return ig_g(p + 1);
        case 4: return ig_g(p + 2);
        case 5: return ig_g(p * 3);
        case 6: return sarr[p & 63] + 1;
        case 7: return fptab[p & 7](p);
        case 8: return (int)CORE.f[p & 31];
        default: break;
    }
    return 0;
}

long long ig_wide(long long i)
{
    return (long long)sarr[i & 63] + (long long)CORE.q[i & 15];
}
