#include "common.h"
namespace { int s_count = 0; }
void f(int* a, int n) { static Obj x = make(1); Ext e{a[0]}; x.hits += e.bump() + s_count; ++s_count; sink(x.hits); (void)n; }
void g() { sink(1); }
