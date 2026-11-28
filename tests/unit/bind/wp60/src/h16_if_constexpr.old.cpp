#include "common.h"
#include "h16.h"
namespace { int s_count = 0; }
namespace { struct Cfg { static constexpr bool kHas = true; }; }
void f(int* a, int n) { ++s_count; sink(tc<Cfg>(a[0]) + s_count); (void)n; }
void g() { sink(1); }
