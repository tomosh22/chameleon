#include "common.h"
namespace { int s_count = 0; struct Local { int k; }; }
struct Registry { template <class T> static Obj& get() { static Obj x = make(1); return x; } };
void f(int* a, int n) { ++s_count; Obj& o = Registry::get<Local>(); o.hits += a[0] + s_count; sink(o.hits); (void)n; }
void g() { sink(1); }
