#include "common.h"
// WP-56: R-61 two lambdas down. f's own `static Obj nested` (a plain record of f), its nested lambdas without statics.
namespace {
int s_nest_made = 0;
int s_nest_calls = 0;
}  // namespace
int g_o2_nest = 0;
Obj o2_nest_make(int id) { ++s_nest_made; Obj n; n.v = id; n.hits = 0; return n; }
void f()
{
    static Obj nested = o2_nest_make(700000);
    auto outer = []() {
        auto inner = [] { return 1; };
        return inner() + 1;
    };
    nested.hits += outer();
    g_o2_nest = nested.v + nested.hits;
    ++s_nest_calls;
    sink(g_o2_nest);
}
int o2_nest_mark() { return 1; }
