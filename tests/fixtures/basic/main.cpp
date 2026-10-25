// Fixture `basic` main loop: prints `TICK n=<k> value=<compute_value()> extras=<n>` every 100 ms.
// `--ticks N` stops after N lines (for ctest); without it the loop runs until killed.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "extras.h"
#include "logic.h"

// Externals used by logic.cpp (bind-to-old data and function in the hot-reload scenario).
int g_tick = 0;

void fixture_note(const char* what)
{
    std::printf("NOTE %s\n", what);
}

int main(int argc, char** argv)
{
    long max_ticks = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            max_ticks = std::strtol(argv[++i], nullptr, 10);
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    while (max_ticks < 0 || g_tick < max_ticks) {
        std::printf("TICK n=%d value=%d extras=%d\n", g_tick, compute_value(), extras_value());
        ++g_tick;
        Sleep(100);
    }
    return 0;
}
