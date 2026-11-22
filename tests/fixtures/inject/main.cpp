// Fixture `inject` (WP-28): prints `TICK n=<k> value=<compute_value()> calls=<g_calls>` every 100 ms.
//
// g_calls is incremented by compute_value() itself and lives in this file, which no edit touches: it keeps counting through
// every reload, failed or not, so a scenario can show that the application never stopped and never lost its state.
#include <cstdio>

#include "fixture_runtime.h"
#include "logic.h"

int g_calls = 0;

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        const int value = compute_value();
        std::printf("TICK n=%d value=%d calls=%d\n", n, value, g_calls);
    });
}
