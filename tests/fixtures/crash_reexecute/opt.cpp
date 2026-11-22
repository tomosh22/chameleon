// Built WITH optimisation (CMakeLists.txt: /O2 for this source in every configuration). Crashes like step(), but re-execute must
// not be offered for it: an optimised function does not keep its parameters in fixed places.
#include "logic.h"

__declspec(noinline) int opt_step(int* p, int k)
{
    volatile int* q = p;
    const int v = *q;
    return v + k;
}
