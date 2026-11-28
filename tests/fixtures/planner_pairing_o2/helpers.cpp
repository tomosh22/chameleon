// WP-60 (R-57, ADR-0047): an own static of a function that inlined a `static` helper. clang-cl at /O2 records the statics of an
// inlined helper as plain records of the caller, in the shape of the caller's own, so the planner pairs the caller's own static
// only when the caller's code proves it (WP-57's proof): o2_help_step() inlines help_scale(), a helper without statics, and the
// edit help_6 changes o2_help_mark() only, so o2_help_step() is the old code and `own` keeps the running object. s_help_made and
// s_help_calls are ordinary statics of the file: under clang-cl they teach the layout the record-less guard of `own` is placed
// from (a pairing that only the proof can keep teaches none).
#include "o2.h"

namespace {
int s_help_made = 0;
int s_help_calls = 0;

__declspec(noinline) O2Name help_make(int id)
{
    ++s_help_made;
    O2Name n;
    n.id = id;
    n.hits = 0;
    return n;
}

}  // namespace

int g_help_value = 0;
int g_help_scaled = 0;

static int help_scale(int v)
{
    return v * 2 + s_help_calls;
}

void o2_help_step()
{
    static O2Name own = help_make(950000);
    own.hits += 1;
    ++s_help_calls;
    g_help_scaled = help_scale(own.hits);
    g_help_value = own.id + own.hits;
}

int o2_help_value()
{
    return g_help_value;
}

int o2_help_made()
{
    return s_help_made;
}

int o2_help_mark()
{
    return 1;
}
