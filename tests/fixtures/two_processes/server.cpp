// Fixture `two_processes`, server: prints `TICK n=<k> role=server value=<tp_value()> calls=<n>`.
#include <cstdio>

#include "fixture_runtime.h"
#include "lib.h"

int main(int argc, char** argv)
{
    cml_fixture::extra_modules().push_back(L"fixture_two_processes_lib.dll");
    return cml_fixture::run(argc, argv, [](int n) {
        const int value = tp_value();
        std::printf("TICK n=%d role=server value=%d calls=%d\n", n, value, tp_calls());
    });
}
