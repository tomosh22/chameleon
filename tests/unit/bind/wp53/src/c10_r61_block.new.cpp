#include "common.h"
namespace { int s_count = 0; }
void f() { auto l = [](int v) -> Obj& { sink(v); static Obj x = make(1); x.hits += v; return x; }; ++s_count; sink(l(s_count).hits); }
