#pragma once

// A header with `static inline` overload sets, in the shape of Dear ImGui's imgui_internal.h
// (`static inline float ImFloor(float)` / `static inline ImVec2 ImFloor(const ImVec2&)`). Every translation unit that
// includes it gets its own internal-linkage copies, and the PDB records each as an S_LPROC32 named just `so_floor`:
// only the function's type tells the overloads apart (WP-37, ADR-0042).

struct SoVec2 {
    float x;
    float y;
};

// Patched by the `static_overloads` scenario: the float overload gains + 100.
static inline float so_floor(float f)
{
    return static_cast<float>(static_cast<int>(f));
}

static inline SoVec2 so_floor(const SoVec2& v)
{
    SoVec2 r;
    r.x = static_cast<float>(static_cast<int>(v.x));
    r.y = static_cast<float>(static_cast<int>(v.y));
    return r;
}

// A second set with three members; none of them is edited.
static inline int so_weight(int v)
{
    return v + 1;
}

static inline int so_weight(long long v)
{
    return static_cast<int>(v) + 2;
}

static inline int so_weight(const char* s)
{
    return s[0] == '\0' ? 0 : 3;
}

// end of overloads.h
