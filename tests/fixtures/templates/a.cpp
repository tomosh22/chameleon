#include "twice.h"

// TU-local template with int and double instantiations.
template <typename T> __declspec(noinline) T offset(T v)
{
    return v + 1;
}

int templates_a()
{
    return twice(3) + offset(4) + static_cast<int>(offset(1.5));
}
