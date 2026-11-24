// Fixture `lambda_statics` (WP-37): prints
//   TICK n=<k> mark=<ls_step()> a=<n> b=<n> c=<n> oi=<ls_over(0)> od=<ls_over(0.0)> made=<ls_constructions()>
// a counts up from 100001 by 1, b from 200002 by 2, c from 300003 by 3, oi from 400001, od from 500001; made stays 5.
#include <cstdio>

#include "fixture_runtime.h"
#include "names.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        const int mark = ls_step();
        const int oi = ls_over(0);
        const int od = ls_over(0.0);
        std::printf("TICK n=%d mark=%d a=%d b=%d c=%d oi=%d od=%d made=%d\n", n, mark, ls_a(), ls_b(), ls_c(), oi, od, ls_constructions());
    });
}
