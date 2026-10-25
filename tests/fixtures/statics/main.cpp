// Fixture `statics`: prints `TICK n=<k> value=<statics_value()> calls=<n> total=<n>`.
#include <cstdio>

#include "fixture_runtime.h"
#include "statics.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        const int value = statics_value();
        std::printf("TICK n=%d value=%d calls=%d total=%d\n", n, value, statics_calls(), statics_total());
    });
}
