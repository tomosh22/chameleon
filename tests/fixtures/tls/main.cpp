// Fixture `tls`: the main thread and a worker thread each call tls_step() once per 100 ms frame; every thread owns its
// thread_locals. Prints `TICK n=<k> step=<s> main_ns=<v> main_fn=<v> work_ns=<v> work_fn=<v>`.
#include <atomic>
#include <cstdio>
#include <thread>

#include <windows.h>

#include "fixture_runtime.h"
#include "logic.h"

namespace {

std::atomic<int> g_work_ns{0};
std::atomic<int> g_work_fn{0};

void worker()
{
    for (;;) {
        const TlsValues v = tls_step();
        g_work_ns.store(v.ns);
        g_work_fn.store(v.fn);
        Sleep(100);
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::thread(worker).detach();
    return cml_fixture::run(argc, argv, [](int n) {
        const TlsValues v = tls_step();
        std::printf("TICK n=%d step=%d main_ns=%d main_fn=%d work_ns=%d work_fn=%d\n", n, v.step, v.ns, v.fn, g_work_ns.load(),
                    g_work_fn.load());
    });
}
