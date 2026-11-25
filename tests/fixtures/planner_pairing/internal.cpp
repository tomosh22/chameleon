#include "pairing.h"

namespace {
int s_internal_calls = 0;  // file-scope state next to the statics (it teaches clang-cl the layout of their section)
}  // namespace

// R-48 item 11: a function with internal linkage. cl records its statics under a qualified name ("`pp_opts'::`2'::names");
// until WP-40 the by-place step compared the whole record name with "names" and refused the two under cl.
static int pp_opts(int idx)
{
    int value = 0;
    if (idx == 0) {
        static PpObj names = pp_make(200000);
        names.hits += 1;
        value = names.id + names.hits;
    } else if (idx == 1) {
        static PpObj names = pp_make(250000);
        names.hits += 1;
        value = names.id + names.hits;
    }
    return value;
}

int pp_internal(int idx)
{
    return pp_opts(idx);
}

int pp_internal_mark()
{
    ++s_internal_calls;
    return s_internal_calls > 0 ? 1 : 0;
}
