#pragma once

#include "scale.h"

// An inline function in a header, used by both tu_a.cpp and tu_b.cpp. Each TU has its own COMDAT copy.
inline __declspec(noinline) int shared_scale()
{
    return kScale;
}
