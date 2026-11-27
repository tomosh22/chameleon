#pragma once

#include "scale.h"

// An inline function in a header, used by tu_a.cpp, tu_b.cpp and tu_c.cpp. Each TU has its own COMDAT copy; the reload of the
// first hooks it, the next two compare their copy with that staged patch (WP-47) and add nothing for it.
inline __declspec(noinline) int shared_scale()
{
    return kScale;
}
