// Fixture `lambdas`: prints `TICK n=<k> value=<lambdas_value(n)>`.
#include <cstdio>

#include "fixture_runtime.h"
#include "lambdas.h"

int main(int argc, char** argv)
{
    lambdas_init(argc > 0 ? 5 : 0);  // runtime value, so the capture is real
    return cml_fixture::run(argc, argv, [](int n) {
        std::printf("TICK n=%d value=%d\n", n, lambdas_value(n));
    });
}
