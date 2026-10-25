#include "pch.h"  // /Yu

#include "logic.h"

// Exactly two externals, both defined in main.cpp (same shape as fixture `basic`).
extern int g_tick;
void fixture_note(const char* what);

int compute_value()
{
    if (g_tick == 0) {
        fixture_note("logic-start");
    }
    // Uses types from the precompiled header so that the TU really depends on it.
    const PchPoint p = {pch_base() - 99, 0};
    const std::vector<int> values(static_cast<std::size_t>(p.x), p.y);
    return static_cast<int>(values.size());
}
