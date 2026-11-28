// Fixture `planner_pairing_o2` (WP-40): prints
//   TICK n=<k> mark=<o2_mark()> a=<n> b=<n> c=<n> parts=<n> pm=<o2_parts_mark()> made=<o2_made()> nest=<n> nmade=<n> nmark=<n>
//        bits=<n> bsec=<n> bmade=<n> bmark=<n> nest2=<n> n2made=<n>
// a / b / c are the ids 100000 / 200000 / 300000 of three lambdas' statics plus their counts, parts is 500000 plus the sum
// of the split array; made stays 3 (no static is constructed twice). nest is 700000 plus the count of the nested lambda's
// static (WP-52), nmade stays 1, nmark is o2_nest_mark(). bits / bsec are 800000 / 900000 plus the counts of an inline
// function's two statics that share one bitmask (WP-59), bmade stays 2, bmark is o2_bits_mark(). nest2 is 600000 plus the
// count of the nested lambda's static inlined all the way (WP-56), n2made stays 1.
#include <cstdio>

#include "fixture_runtime.h"
#include "o2.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        o2_step();
        o2_parts_step();
        o2_nest_step();
        o2_bits_step();
        o2_nest2_step();
        std::printf("TICK n=%d mark=%d a=%d b=%d c=%d parts=%d pm=%d made=%d nest=%d nmade=%d nmark=%d bits=%d bsec=%d bmade=%d bmark=%d nest2=%d n2made=%d\n", n,
                    o2_mark(), o2_value(0), o2_value(1), o2_value(2), o2_parts_sum(), o2_parts_mark(), o2_made(), o2_nest_value(), o2_nest_made(), o2_nest_mark(),
                    o2_bits_value(), o2_bits_second(), o2_bits_made(), o2_bits_mark(), o2_nest2_value(), o2_nest2_made());
    });
}
