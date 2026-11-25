// Fixture `planner_pairing_o2` (WP-40): prints
//   TICK n=<k> mark=<o2_mark()> a=<n> b=<n> c=<n> parts=<n> pm=<o2_parts_mark()> made=<o2_made()>
// a / b / c are the ids 100000 / 200000 / 300000 of three lambdas' statics plus their counts, parts is 500000 plus the sum
// of the split array; made stays 3 (no static is constructed twice).
#include <cstdio>

#include "fixture_runtime.h"
#include "o2.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        o2_step();
        o2_parts_step();
        std::printf("TICK n=%d mark=%d a=%d b=%d c=%d parts=%d pm=%d made=%d\n", n, o2_mark(), o2_value(0), o2_value(1), o2_value(2), o2_parts_sum(), o2_parts_mark(),
                    o2_made());
    });
}
