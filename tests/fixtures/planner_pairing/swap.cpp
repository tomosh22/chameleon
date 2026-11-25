#include "pairing.h"

namespace {
int s_a = 0;
int s_b = 0;
}  // namespace

// R-48 item 5: two lambdas, each with a static `slot`. The first takes no argument, the second an int. The edit swaps the
// two statements, so the compiler numbers the lambdas the other way round: the number no longer names the same lambda.
void pp_swap_step()
{
    PpObj& a = ([]() -> PpObj& { static PpObj slot = pp_make(300000); return slot; })();
    PpObj& b = ([](int k) -> PpObj& { static PpObj slot = pp_make(350000 + k); return slot; })(0);
    a.hits += 1;
    b.hits += 1;
    s_a = a.id + a.hits;
    s_b = b.id + b.hits;
}

int pp_swap_a()
{
    return s_a;
}

int pp_swap_b()
{
    return s_b;
}
