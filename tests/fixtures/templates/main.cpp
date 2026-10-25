// Fixture `templates`: prints `TICK n=<k> value=<templates_a()> other=<templates_b()>`.
#include <cstdio>

#include "fixture_runtime.h"
#include "twice.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        std::printf("TICK n=%d value=%d other=%d\n", n, templates_a(), templates_b());
    });
}
