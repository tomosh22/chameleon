// Fixture `cross_module_atomic` (WP-54) main loop: prints `TICK n=<k> exe=<exe_value()> plugin=<plugin_value()> frames=<f>` every
// 100 ms and enables the exe and the DLL. Never patched. `frames` is the DLL's thread_local count of the main thread's calls: it only
// ever grows while the process runs.
#include <cstdio>

#include "exe_part.h"
#include "fixture_runtime.h"
#include "plugin.h"

int main(int argc, char** argv)
{
    cml_fixture::extra_modules().push_back(L"fixture_cross_module_atomic_plugin.dll");
    return cml_fixture::run(argc, argv, [](int n) {
        std::printf("TICK n=%d exe=%d plugin=%d frames=%d\n", n, exe_value(), plugin_value(), plugin_frames());
    });
}
