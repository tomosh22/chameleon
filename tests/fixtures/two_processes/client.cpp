// Fixture `two_processes`, client: prints `TICK n=<k> role=client value=<tp_value()> calls=<n>`. It calls the DLL twice
// per tick, so its counter differs from the server's: each process has its own copy of the DLL's state.
#include <cstdio>

#include "fixture_runtime.h"
#include "lib.h"

int main(int argc, char** argv)
{
    cml_fixture::extra_modules().push_back(L"fixture_two_processes_lib.dll");
    return cml_fixture::run(argc, argv, [](int n) {
        tp_value();
        const int value = tp_value();
        std::printf("TICK n=%d role=client value=%d calls=%d\n", n, value, tp_calls());
    });
}
