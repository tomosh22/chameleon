// Fixture `legacy_lambdas` (WP-46): prints
//   TICK n=<k> mark=<ll_mark()> a=<n> b=<n> c=<n> made=<ll_made()>
// a / b / c are the ids 100000 / 200000 / 300000 of three lambdas' statics plus their counts; made stays 3 (no static is
// constructed twice).
#include <cstdio>

#include "fixture_runtime.h"
#include "legacy.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        const int a = ll_a();
        const int b = ll_b();
        const int c = ll_c();
        std::printf("TICK n=%d mark=%d a=%d b=%d c=%d made=%d\n", n, ll_mark(), a, b, c, ll_made());
    });
}
