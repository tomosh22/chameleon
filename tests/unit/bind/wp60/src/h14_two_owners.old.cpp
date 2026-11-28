#include "common.h"
namespace { int s_count = 0; }
void f(int* a, int n) { static Obj x = make(1); x.hits += ext_count() + a[0] + s_count; ++s_count; sink(x.hits); (void)n; }
void k() { static Obj x = make(3); x.hits += ext_count(); sink(x.hits); }
void g() { sink(1); }
