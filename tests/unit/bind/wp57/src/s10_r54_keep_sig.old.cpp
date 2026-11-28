#include "common.h"
namespace { int s_count = 0; }
Flag s_flag;
void f(int* a, int n) { auto l = [](int v) { static Obj x = make(1); x.hits += v; return x.hits; }; sink(l(a[0]) + s_count); ++s_count; (void)n; }
void g() { sink(1); }
