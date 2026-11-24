#include "overloads.h"
#include "tus.h"

namespace {
int s_calls = 0;  // lives in the OLD image: a patch with its own copy would restart at 0
}

int a_float()
{
    ++s_calls;
    return static_cast<int>(so_floor(7.9f + static_cast<float>(g_so_zero)));
}

int a_vec()
{
    const SoVec2 v = {3.7f + static_cast<float>(g_so_zero), 5.2f};
    const SoVec2 r = so_floor(v);
    return static_cast<int>(r.x) * 10 + static_cast<int>(r.y);
}

int a_weight()
{
    return so_weight(10 + g_so_zero) + so_weight(20LL + g_so_zero) + so_weight(g_so_zero != 0 ? "" : "x");
}

int a_calls()
{
    return s_calls;
}
