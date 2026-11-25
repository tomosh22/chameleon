#include "overloads.h"
#include "tus.h"

namespace {
int s_calls = 0;
}

int b_float()
{
    ++s_calls;
    return static_cast<int>(so_floor(21.5f + static_cast<float>(g_so_zero)));
}

int b_vec()
{
    const SoVec2 v = {8.1f + static_cast<float>(g_so_zero), 2.9f};
    const SoVec2 r = so_floor(v);
    return static_cast<int>(r.x) * 10 + static_cast<int>(r.y);
}

int b_calls()
{
    return s_calls;
}
