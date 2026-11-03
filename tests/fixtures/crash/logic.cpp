#include "logic.h"

int cell_store(int value)
{
    volatile int* cell = reinterpret_cast<volatile int*>(g_cell);
    *cell = value;
    return *cell;
}

void maintain()
{
    g_maintain_calls.fetch_add(1);
}
