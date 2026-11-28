#include "common.h"
#include "h16.h"
namespace { int s_count = 0; }
namespace { struct Cfg { static constexpr bool kHas = false; }; }
void f(int* a, int n) { ++s_count; static int x = 0; x += a[0]; sink(tc<Cfg>(x) + s_count); (void)n; }
void g() { sink(1); }
