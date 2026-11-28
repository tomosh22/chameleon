// Fixture `inlined_owner_o2` (WP-57): prints `TICK n=<k> mark=<io_mark()> count=<n> made=<io_made()> top=<io_top()>`.
// count is 800000 plus the number of io_step() calls since the static was constructed; made stays 1; top is the largest
// value io_step() sorted (29 or 30).
#include <cstdio>

#include "fixture_runtime.h"
#include "inlined_owner.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        const int count = io_step(n);
        std::printf("TICK n=%d mark=%d count=%d made=%d top=%d\n", n, io_mark(), count, io_made(), io_top());
    });
}
