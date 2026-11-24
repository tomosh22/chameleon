// Fixture `lambda_statics` (WP-37): prints
//   TICK n=<k> mark=<ls_step()> a=<n> b=<n> c=<n> oi=<ls_over(0)> od=<ls_over(0.0)> tf=<n> tt=<n> nx=<n> ny=<n> p0=<n> p1=<n> made=<ls_constructions()>
// Every value is the id of one function-local static plus how often it was fetched: a counts up from 100001, b from
// 200002 by 2, c from 300003 by 3, oi from 400001, od from 500001, tf (LsTree::flush) from 600001, tt (LsTree::timers)
// from 700001, nx / ny (the anonymous-namespace function of anon.cpp) from 800001 and 900002 by 2, p0 / p1 (the two
// `names` of LsTree::options) from 1000001 and 1100001. made stays 11: nothing is constructed twice.
#include <cstdio>

#include "fixture_runtime.h"
#include "names.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        LsTree tree;
        const int mark = ls_step();
        const int oi = ls_over(0);
        const int od = ls_over(0.0);
        const int tf = tree.flush(0);
        const int tt = tree.timers(0, true);
        ls_anon_step();
        const int p0 = tree.options(0);
        const int p1 = tree.options(1);
        std::printf("TICK n=%d mark=%d a=%d b=%d c=%d oi=%d od=%d tf=%d tt=%d nx=%d ny=%d p0=%d p1=%d made=%d\n", n, mark, ls_a(), ls_b(), ls_c(), oi, od, tf, tt,
                    ls_anon_x(), ls_anon_y(), p0, p1, ls_constructions());
    });
}
