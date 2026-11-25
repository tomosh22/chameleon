#include "pch.h"  // /Yu

#include "logic.h"

extern int g_tick;  // main.cpp; never negative

// WP-37 (ADR-0042): a static overload set in a translation unit that uses the precompiled header. Two of the three
// signatures need types that come out of the precompiled header (PchPoint, std::string). Under cl the project's /Zi
// PCH makes the patch compile keep /Zi with a private compiler PDB (ADR-0026), so the patch object's .debug$T holds
// only a type-server reference and the planner reads the types from that PDB.
static int pch_pick(int v)
{
    return v + 1;
}

static int pch_pick(const PchPoint& p)
{
    return p.x * 10 + p.y;
}

static int pch_pick(const std::string& s)
{
    return static_cast<int>(s.size()) * 100;
}

// 6 + 34 + 200 = 240; the `pch_static_overloads` scenario patches pch_pick(int) and gets 1240.
int pch_overloads()
{
    // Arguments that depend on a run-time value, so that an optimising compiler keeps the three functions.
    const int never = g_tick < 0 ? 1 : 0;
    const PchPoint p = {3 + never, 4};
    return pch_pick(5 + never) + pch_pick(p) + pch_pick(std::string(never != 0 ? "abc" : "ab"));
}
