#include "pairing.h"

namespace {
int s_made = 0;
int s_ticks = 0;  // file-scope state in the same section as the statics below (it teaches clang-cl's section layout)
int s_other = 0;  // more live data of this translation unit, non-zero at run time
}  // namespace

PpObj pp_make(int id)
{
    ++s_made;
    PpObj o;
    o.id = id;
    o.hits = 0;
    return o;
}

int pp_made()
{
    return s_made;
}

int pp_guards_tick()
{
    ++s_ticks;
    s_other += 3;
    return 50000 + s_ticks + (s_other > 0 ? 0 : 1);
}

// R-48 item 2: the edit gives this function a function-local static with a dynamic initialiser. Its guard is new; until
// WP-40 the clang-cl layout step could place it on old data of this unit (s_other, never zero), and the static would
// never have been constructed.
int pp_fresh()
{
    return 1;
}
