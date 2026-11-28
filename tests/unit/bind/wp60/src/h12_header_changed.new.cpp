#include "common.h"
#include "h12_new.h"
namespace { int s_count = 0; }
void f(int* a, int n) { static Obj x = make(1); x.hits += hx(a[0]) + s_count; ++s_count; sink(x.hits); (void)n; }
void g() { sink(1); }
