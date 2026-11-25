#include "pairing.h"

namespace {
int s_twin_a = 0;
int s_twin_b = 0;
}  // namespace

// R-48 item 5, ADR-0043 rule (d): two lambdas that nothing in the type records tells apart (no captures, the same call
// operator `() -> PpObj&`), each with a static `slot`, in the shape of Godot's SNAME. The edit swaps the two statements: the
// compiler numbers them the other way round. The code of the call operators (the id each passes) shows which old lambda
// each new one is, and the statics follow the code, not the numbers: `ta=` stays in 400000's range, `tb=` in 450000's.
void pp_twins_step()
{
    PpObj& a = ([]() -> PpObj& { static PpObj slot = pp_make(400000); return slot; })();
    PpObj& b = ([]() -> PpObj& { static PpObj slot = pp_make(450000); return slot; })();
    a.hits += 1;
    b.hits += 2;
    s_twin_a = a.id + a.hits;
    s_twin_b = b.id + b.hits;
}

int pp_twins_a()
{
    return s_twin_a;
}

int pp_twins_b()
{
    return s_twin_b;
}
