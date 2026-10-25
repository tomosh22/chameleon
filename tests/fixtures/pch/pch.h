#pragma once

// The precompiled header of fixture `pch`. Everything the translation units share goes here;
// changing it must be rejected by the hot-reload pipeline (CML2101), see edits/pch_header.edit.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Chameleon_API.h"

#define PCH_BASE 100

inline int pch_base()
{
    return PCH_BASE;
}

struct PchPoint {
    int x;
    int y;
};
