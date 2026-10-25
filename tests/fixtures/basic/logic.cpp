#include "logic.h"

// Exactly two externals, both defined in main.cpp: one data symbol, one function.
extern int g_tick;
void fixture_note(const char* what);

int compute_value()
{
    if (g_tick == 0) {
        fixture_note("logic-start");
    }
    return 1;
}
