// WP-60 (R-57, ADR-0047): the static `rx` of a `static` helper, r57_name(), inlined into o2_r57_step() (__forceinline, so that
// both compilers inline it at /O2). clang-cl records `rx` as a plain record of o2_r57_step(). The edit r57_7 deletes the helper
// and declares `static O2Name rx` in o2_r57_step() itself, with another initialiser: until WP-60 the new static was paired with
// the helper's record and its guard placed by the layout, so the patch took the helper's live object (r57 kept counting from
// 960000, its initialiser never ran). Now the caller's inline sites name a helper whose statics cannot be read, so the static
// is kept only if o2_r57_step() is the old code, which it is not: refused (CML3207). The edit keeps the order of the globals,
// so the layout s_r57_calls teaches still places the guard (the refusal must come from the rule, not CML3600): s_r57_calls is
// touched first on both sides, and the construction counter is external (emitted in the order it is defined).
#include "o2.h"

int g_r57_value = 0;
int g_r57_made = 0;

namespace {
int s_r57_calls = 0;

__declspec(noinline) O2Name r57_make(int id)
{
    ++g_r57_made;
    O2Name n;
    n.id = id;
    n.hits = 0;
    return n;
}

}  // namespace

static __forceinline O2Name& r57_name()
{
    static O2Name rx = r57_make(960000);
    return rx;
}

void o2_r57_step()
{
    ++s_r57_calls;
    O2Name& n = r57_name();
    n.hits += 1;
    g_r57_value = n.id + n.hits;
}

int o2_r57_value()
{
    return g_r57_value + (s_r57_calls > 0 ? 0 : 1);
}

int o2_r57_made()
{
    return g_r57_made;
}

int o2_r57_mark()
{
    return 1;
}
