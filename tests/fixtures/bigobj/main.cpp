// Fixture `bigobj` main loop: prints `TICK n=<k> value=<bigobj_sum(k)>` every 100 ms.
// `--ticks N` stops after N lines (for ctest); without it the loop runs until killed.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int bigobj_sum(int x);

int g_bigobj_sink = 0;

int main(int argc, char** argv)
{
    long max_ticks = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            max_ticks = std::strtol(argv[++i], nullptr, 10);
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    int tick = 0;
    while (max_ticks < 0 || tick < max_ticks) {
        std::printf("TICK n=%d value=%d\n", tick, bigobj_sum(tick));
        ++tick;
        Sleep(100);
    }
    return 0;
}
