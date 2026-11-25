// Fixture `legacy_lambdas` (WP-46): prints
//   TICK n=<k> cc=<cl|clang> mark=<ll_mark()> a=<n> b=<n> c=<n> made=<ll_made()>
// a / b / c are the ids 100000 / 200000 / 300000 of three lambdas' statics plus their counts (edits add an offset to what a
// lambda returns: 50000, 70000); made stays 3 (no static is constructed twice). `cc` lets the scenario expect cl's refusals
// (lambdas named by a hash of their code) and clang-cl's reloads (numbered lambdas) in one step.
#include <cstdio>

#include "fixture_runtime.h"
#include "legacy.h"

int main(int argc, char** argv)
{
#if defined(__clang__)
    const char* const cc = "clang";
#else
    const char* const cc = "cl";
#endif
    return cml_fixture::run(argc, argv, [cc](int n) {
        const int a = ll_a();
        const int b = ll_b();
        const int c = ll_c();
        std::printf("TICK n=%d cc=%s mark=%d a=%d b=%d c=%d made=%d\n", n, cc, ll_mark(), a, b, c, ll_made());
    });
}
