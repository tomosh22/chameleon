// Fixture `guard_bits` (WP-63): prints
//   TICK n=<k> nts=<n> ntsx=<n> nmark=<n> ts=<n> tsx=<n> tmark=<n> made=<n>
// nts / ts are 100000 / 200000 plus the calls of two inline functions' constant-initialised statics (they never drop across
// a reload); ntsx / tsx are id + hits of the dynamic statics the edits add (300000 / 400000 + n, 0 before); made counts
// their constructions (0, then 1, then 2); nmark / tmark show the patched code runs.
#include <cstdio>

#include "fixture_runtime.h"
#include "guard_bits.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        gb_nts_step();
        gb_ts_step();
        std::printf("TICK n=%d nts=%d ntsx=%d nmark=%d ts=%d tsx=%d tmark=%d made=%d\n", n, gb_nts_value(), gb_nts_extra(), gb_nts_mark(), gb_ts_value(),
                    gb_ts_extra(), gb_ts_mark(), gb_made());
    });
}
