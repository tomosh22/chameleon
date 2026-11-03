#include "elision.h"

// TU-local template; both instantiations are COMDAT functions the edits never touch.
template <typename T> __declspec(noinline) T scale(T v)
{
    return static_cast<T>(v * 3);
}

__declspec(noinline) int elision_base()
{
    return 10;
}

__declspec(noinline) int elision_tmpl()
{
    return scale<int>(4) + scale<short>(2);
}

// A string literal: a fresh read-only constant of the patch, compared by content.
__declspec(noinline) const char* elision_text()
{
    return "alpha";
}

// Takes a value so that the optimiser cannot fold its result into the caller (it would then be the caller that changes).
__declspec(noinline) int elision_inner(int x)
{
    return x + 5;
}

// A small function that calls another: the call must still resolve to the same target.
__declspec(noinline) int elision_outer(int x)
{
    return elision_inner(x) + 1;
}
