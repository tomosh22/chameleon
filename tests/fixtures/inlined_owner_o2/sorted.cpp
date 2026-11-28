// Fixture `inlined_owner_o2` (WP-57, R-59): an own `static` of a function that sorts with a lambda.
// io_step() holds `static Counter counter` (constructed once: io_made() stays 1) and calls std::sort with a lambda, which
// /O2 inlines into io_step() (std::sort, a template over the closure) and into the out-of-line instantiation of the sort
// loop. The edits change io_mark() only, so io_step() compiles to the old code and its static keeps counting.
#include <algorithm>

#include "inlined_owner.h"

namespace {

struct Counter {
    int base;
    int count;
};

int s_made = 0;
int s_top = 0;

__declspec(noinline) Counter make_counter(int base)
{
    ++s_made;
    return Counter{base, 0};
}

}  // namespace

int io_step(int n)
{
    static Counter counter = make_counter(800000);
    int v[24];
    for (int i = 0; i < 24; ++i) {
        v[i] = (n * 7 + i * 5) % 31;
    }
    std::sort(v, v + 24, [](int l, int r) { return l > r; });
    s_top = v[0];
    counter.count += 1;
    return counter.base + counter.count;
}

int io_made()
{
    return s_made;
}

int io_top()
{
    return s_top;
}

int io_mark()
{
    return 1;
}
