#include "pairing.h"

// R-48 item 3: the edit adds `static int helper(int v)` at file scope. Until WP-40 the planner hooked the old
// PpScope::helper onto that new function (the names match in their last component only): main.cpp's call would have
// returned 7000 instead of 2.
int PpScope::helper(int v)
{
    return v + 1;
}

int pp_scope_local()
{
    return 10;
}
