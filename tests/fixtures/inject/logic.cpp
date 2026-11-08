#include "logic.h"

// Defined in main.cpp: a patch binds to the running image's storage, so the counter survives every reload.
extern int g_calls;

int compute_value()
{
    ++g_calls;
    return 1;
}
