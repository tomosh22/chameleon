#include "common.h"
namespace { int s_count = 0; }
void f(int* a, int n) { ++s_count; static Obj x = make(1); Obj& o = x; o.hits += a[0] + s_count; sink(o.hits); (void)n; }
void g() { sink(1); }
