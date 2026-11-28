#include "common.h"
namespace { int s_count = 0; }
Flag s_flag;
void f(int* a, int n) { static Obj x = make(1); auto l = [](int v) { return v * 2; }; x.hits += l(a[1]); sink(x.hits + s_count); ++s_count; (void)n; }
void g() { sink(1); }
