/* Companion of lib.c (F-1): prints game_value() every 100 ms; run it with the agent (see tests/fixtures/common/fixture_runtime.h). */
#include <stdio.h>
#include <windows.h>

int game_value(void);

int main(void) {
    for (int i = 0; i < 1000; ++i) {
        printf("TICK n=%d value=%d\n", i, game_value());
        fflush(stdout);
        Sleep(100);
    }
    return 0;
}
