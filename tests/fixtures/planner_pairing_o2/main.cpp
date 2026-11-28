// Fixture `planner_pairing_o2` (WP-40): prints
//   TICK n=<k> mark=<o2_mark()> a=<n> b=<n> c=<n> parts=<n> pm=<o2_parts_mark()> made=<o2_made()> nest=<n> nmade=<n> nmark=<n>
//        bits=<n> bsec=<n> bmade=<n> bmark=<n> nest2=<n> n2made=<n> help=<n> hmade=<n> hmark=<n> r57=<n> rmade=<n> rmark=<n>
// a / b / c are the ids 100000 / 200000 / 300000 of three lambdas' statics plus their counts, parts is 500000 plus the sum
// of the split array; made stays 3 (no static is constructed twice). nest is 700000 plus the count of the nested lambda's
// static (WP-52), nmade stays 1, nmark is o2_nest_mark(). bits / bsec are 800000 / 900000 plus the counts of an inline
// function's two statics that share one bitmask (WP-59), bmade stays 2, bmark is o2_bits_mark(). nest2 is 600000 plus the
// count of the nested lambda's static inlined all the way (WP-56), n2made stays 1. help is 950000 plus the count of an own
// static next to an inlined helper (WP-60), hmade stays 1, hmark is o2_help_mark(); r57 is 960000 plus the count of an inlined
// helper's static (WP-60), rmade stays 1, rmark is o2_r57_mark().
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
        o2_help_step();
        o2_r57_step();
        std::printf("TICK n=%d mark=%d a=%d b=%d c=%d parts=%d pm=%d made=%d nest=%d nmade=%d nmark=%d bits=%d bsec=%d bmade=%d bmark=%d nest2=%d n2made=%d"
                    " help=%d hmade=%d hmark=%d r57=%d rmade=%d rmark=%d\n", n,
                    o2_mark(), o2_value(0), o2_value(1), o2_value(2), o2_parts_sum(), o2_parts_mark(), o2_made(), o2_nest_value(), o2_nest_made(), o2_nest_mark(),
                    o2_bits_value(), o2_bits_second(), o2_bits_made(), o2_bits_mark(), o2_nest2_value(), o2_nest2_made(),
                    o2_help_value(), o2_help_made(), o2_help_mark(), o2_r57_value(), o2_r57_made(), o2_r57_mark());
    });
}
