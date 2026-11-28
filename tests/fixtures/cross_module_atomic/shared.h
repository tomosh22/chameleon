#pragma once

#include "scale.h"

// An inline function in the header both modules include: the exe and the DLL each have a copy of their own.
inline __declspec(noinline) int shared_scale()
{
    return kScale;
}
