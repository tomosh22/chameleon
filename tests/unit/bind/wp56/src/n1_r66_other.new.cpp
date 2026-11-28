#include "common.h"
// WP-56 (R-66): planner_pairing_o2/nested.cpp without the fixture's noinline. clang-cl -O2 inlines both lambdas into f and
// records the inner lambda's `nested` as a plain record of f, two lambdas up from its call operator.
namespace {
int s_nest_made = 0;
int s_nest_calls = 0;
}  // namespace
int g_o2_nest = 0;
Obj o2_nest_make(int id) { ++s_nest_made; Obj n; n.v = id; n.hits = 0; return n; }
void f()
{
    auto outer = []() {
        auto inner = [] {
            static Obj nested = o2_nest_make(700000);
            return &nested;
        };
        Obj* n = inner();
        n->hits += 1;
        return n->v + n->hits;
    };
    g_o2_nest = outer();
    ++s_nest_calls;
    sink(g_o2_nest);
}
int o2_nest_mark() { return 4; }  // nest_4: another function edited
