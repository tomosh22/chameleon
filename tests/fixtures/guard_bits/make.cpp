// Fixture `guard_bits` (WP-63): what the inline functions' new statics are made by. The edits never touch this file, so
// gb_made() counts every construction of those statics: 1 after the first reload, 2 after the second (a static constructed
// again would make it 3).
#include "guard_bits.h"

namespace {
int s_made = 0;
}  // namespace

__declspec(noinline) GbName gb_make(int id)
{
    ++s_made;
    GbName n;
    n.id = id;
    n.hits = 0;
    return n;
}

int gb_made()
{
    return s_made;
}
