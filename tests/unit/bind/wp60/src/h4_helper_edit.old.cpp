#include "common.h"
namespace { int s_count = 0; }
static int hs(int v) { return v * 3 + s_count; }
void f(int* a, int n) { static Obj x = make(1); x.hits += hs(a[0]); ++s_count; sink(x.hits); (void)n; }
void g() { sink(1); }
