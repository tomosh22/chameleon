#include "pairing.h"

namespace {
int s_sibling_calls = 0;
}  // namespace

// R-48 item 8: the edit adds a new guarded static `extra` to the first block. Whether its guard is extra's or the old
// `names`'s (cl and clang-cl number guards per function: a static declared in front of another takes its number) cannot
// be told from the names, so the reload is refused (cl: CML3207 from the by-place step, clang-cl: CML3600).
int pp_sibling(int idx)
{
    ++s_sibling_calls;
    int value = 0;
    if (idx == 0) {
        static PpObj names = pp_make(40000);
        names.hits += 1;
        value = names.id + names.hits;
    } else if (idx == 1) {
        static PpObj names = pp_make(41000);
        names.hits += 1;
        value = names.id + names.hits;
    }
    return value + (s_sibling_calls > 0 ? 0 : 1);
}
