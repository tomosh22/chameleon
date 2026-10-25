#include "lambdas.h"

#include <functional>

namespace {
std::function<int(int)> g_fn;
}

void lambdas_init(int base)
{
    g_fn = [base](int x) { return x + base; };
}

int lambdas_value(int n)
{
    return g_fn(n % 3);
}
