// Fixture `planner_pairing` (WP-40): prints
//   TICK n=<k> member=<PpScope::helper(1)> local=<pp_scope_local()> fresh=<pp_fresh()> gt=<n> s0=<n> s1=<n> sm=<pp_tsi_mark()>
//        i0=<n> i1=<n> im=<pp_internal_mark()> la=<n> lb=<n> n0=<n> n1=<n> made=<pp_made()>
// Every <n> is the id of one function-local static plus how often it was fetched (see pairing.h). made= counts every
// object ever constructed: 8 before the edits (s0, s1, i0, i1, la, lb, n0, n1), 9 once the new static of pp_fresh() exists.
#include <cstdio>

#include "fixture_runtime.h"
#include "pairing.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        const int member = PpScope::helper(1);
        const int local = pp_scope_local();
        const int fresh = pp_fresh();
        const int gt = pp_guards_tick();
        const int s0 = pp_tsi(0);
        const int s1 = pp_tsi(1);
        const int i0 = pp_internal(0);
        const int i1 = pp_internal(1);
        pp_swap_step();
        const int n0 = pp_sibling(0);
        const int n1 = pp_sibling(1);
        std::printf("TICK n=%d member=%d local=%d fresh=%d gt=%d s0=%d s1=%d sm=%d i0=%d i1=%d im=%d la=%d lb=%d n0=%d n1=%d made=%d\n", n, member, local, fresh, gt, s0, s1,
                    pp_tsi_mark(), i0, i1, pp_internal_mark(), pp_swap_a(), pp_swap_b(), n0, n1, pp_made());
    });
}
