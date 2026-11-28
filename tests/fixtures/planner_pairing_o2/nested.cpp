// WP-52 (R-60, ADR-0045): a static in a nested lambda, compiled with /O2 /Ob2. Both call operators return `auto`, so the
// decorated names of the inner lambda's static and of its guard spell the second `<auto>` by a back-reference, and clang
// 18 numbers that back-reference one lower in the guard than in the static (the guard's own name `$TSS0` is not counted;
// clang 20 and cl count it). Until WP-52 the planner compared the scopes as written: the guard was tied to no static,
// got a fresh guard (warning CML3211) while the static stayed bound, and the patch constructed the live static again.
// Now the scopes are compared with their back-references resolved: the guard is placed with its static, or the reload is
// refused. Either way `nest` keeps counting from where it was and `nmade` stays 1.
//
// The outer lambda is kept out of line under clang (WP-51's repro: the inner one is inlined into it, and clang-cl records
// the static in the outer call operator, where the planner looks for it). cl ignores this; it spells the outer closure in
// the static's name as a back-reference to the name's own table and in the inner closure's unique name to that name's, so
// the closure comparisons see the two alike only with the back-references resolved (WP-56, R-68).
//
// WP-56 (R-66): o2_nest2_step() is the same nest without the noinline. clang-cl inlines both lambdas into it and records
// `nested2` as a plain record of o2_nest2_step(), two lambdas up from its call operator: the planner searches the chain of
// enclosing procedures, and the pairing stands because the edit leaves o2_nest2_step() unchanged (WP-53's proof).
#include "o2.h"

#if defined(__clang__)
#define O2_NEST_NOINLINE __attribute__((noinline))
#else
#define O2_NEST_NOINLINE
#endif

namespace {
int s_nest_made = 0;
int s_nest_calls = 0;  // file-scope state in the section of the static
}  // namespace
int g_o2_nest = 0;  // external on purpose (an internal int could be folded into the code)

O2Name o2_nest_make(int id)
{
    ++s_nest_made;
    O2Name n;
    n.id = id;
    n.hits = 0;
    return n;
}

void o2_nest_step()
{
    auto outer = []() O2_NEST_NOINLINE {
        auto inner = [] {
            static O2Name nested = o2_nest_make(700000);
            return &nested;  // its address escapes: the static is not split (SROA)
        };
        O2Name* n = inner();
        n->hits += 1;
        return n->id + n->hits;
    };
    g_o2_nest = outer();
    ++s_nest_calls;
}

int s_nest2_made = 0;
int g_o2_nest2 = 0;

O2Name o2_nest2_make(int id)
{
    ++s_nest2_made;
    O2Name n;
    n.id = id;
    n.hits = 0;
    return n;
}

void o2_nest2_step()
{
    auto outer = []() {
        auto inner = [] {
            static O2Name nested2 = o2_nest2_make(600000);
            return &nested2;
        };
        O2Name* n = inner();
        n->hits += 1;
        return n->id + n->hits;
    };
    g_o2_nest2 = outer();
}

int o2_nest2_value()
{
    return g_o2_nest2;
}

int o2_nest2_made()
{
    return s_nest2_made;
}

int o2_nest_value()
{
    return g_o2_nest + (s_nest_calls > 0 ? 0 : 1);
}

int o2_nest_made()
{
    return s_nest_made;
}

int o2_nest_mark()
{
    return 1;
}
