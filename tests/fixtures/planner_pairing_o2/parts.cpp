// R-48 item 4: an internal-linkage aggregate that clang-cl at /O2 splits into scalars `s_parts.0`, `.1`, `.2` (every
// access has a constant index). CodeView records no piece, so the old pieces cannot be found. Until WP-40 the planner
// gave the patch fresh pieces without a word (the counters restarted at 0 after a reload of this file); now the reload
// is refused (CML3601). cl keeps the array whole and the reload succeeds.
#include "o2.h"

namespace {
int s_parts[3] = {0, 0, 0};
}  // namespace

void o2_parts_step()
{
    s_parts[0] += 1;
    s_parts[1] += 2;
    s_parts[2] += 3;
}

int o2_parts_sum()
{
    return 500000 + s_parts[0] + s_parts[1] + s_parts[2];
}

int o2_parts_mark()
{
    return 1;
}
