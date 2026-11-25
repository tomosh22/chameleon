#include "pairing.h"

namespace {
int s_tsi_calls = 0;  // file-scope state next to the statics (it teaches clang-cl the layout of their section)
}  // namespace

// Compiled with /Zc:threadSafeInit- (CMakeLists.txt): cl then guards the statics of each block with a bitmask `$S1`
// instead of a thread-safe `$TSSn`, so this function has two guards of ONE name (R-48 item 7). Until WP-40 the planner
// refused them under cl (the guard scan of the by-place step found each `$S1` as an unbound guard of its own block).
int pp_tsi(int idx)
{
    int value = 0;
    if (idx == 0) {
        static PpObj names = pp_make(10000);
        names.hits += 1;
        value = names.id + names.hits;
    } else if (idx == 1) {
        static PpObj names = pp_make(11000);
        names.hits += 1;
        value = names.id + names.hits;
    }
    return value;
}

int pp_tsi_mark()
{
    ++s_tsi_calls;
    return s_tsi_calls > 0 ? 1 : 0;
}
