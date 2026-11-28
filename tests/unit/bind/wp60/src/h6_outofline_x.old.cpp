#include "common.h"
namespace { int s_count = 0; }
static Obj& h() { static Obj x = make(1); return x; }
Obj& (*volatile g_h)() = &h;
void f(int* a, int n) { ++s_count; Obj& o = h(); o.hits += a[0] + s_count; sink(o.hits); (void)n; }
void g() { sink(1); }
