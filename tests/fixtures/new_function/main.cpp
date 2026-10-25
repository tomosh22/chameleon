// Fixture `new_function`: prints `TICK n=<k> value=<compute_value()>`.
#include <cstdio>

#include "fixture_runtime.h"
#include "logic.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        std::printf("TICK n=%d value=%d\n", n, compute_value());
    });
}
