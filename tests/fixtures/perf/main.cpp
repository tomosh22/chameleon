// Fixture `perf`: prints `TICK n=<k> value=<perf_value()> sum=<perf_checksum()>`.
#include <cstdio>

#include "fixture_runtime.h"
#include "perf.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        std::printf("TICK n=%d value=%d sum=%u\n", n, perf_value(), perf_checksum());
    });
}
