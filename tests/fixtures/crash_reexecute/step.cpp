// Built without optimisation (CMakeLists.txt: /Od for this source in every configuration). The function whose frame is re-executed.
#include "logic.h"

int step(int* p, int k, double scale)
{
    g_seen_k = k;
    g_seen_scale = scale;
    int v = *p;
    return v + k + static_cast<int>(scale * 1000.0);
}
