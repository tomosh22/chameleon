// Fixture `elision`: prints `TICK n=<k> base=<> tmpl=<> text=<> chain=<>`.
#include <cstdio>

#include "elision.h"
#include "fixture_runtime.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        std::printf("TICK n=%d base=%d tmpl=%d text=%s chain=%d\n", n, elision_base(), elision_tmpl(), elision_text(), elision_outer());
    });
}
