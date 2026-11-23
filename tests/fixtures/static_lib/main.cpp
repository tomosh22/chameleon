// Fixture `static_lib`: prints `TICK n=<k> value=<static_lib_value()> calls=<n>`. Everything it prints comes from the static library.
#include <cstdio>

#include "fixture_runtime.h"
#include "lib.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        const int value = static_lib_value();
        std::printf("TICK n=%d value=%d calls=%d\n", n, value, static_lib_calls());
    });
}
