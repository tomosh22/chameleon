#include "logic.h"

// Small enough to be inlined into callers when the compiland is optimised. It keeps a standalone body of its own
// because main.cpp calls it through a function pointer.
int small_value()
{
    return 1;
}

int compute_value()
{
    return small_value() + 10;
}
