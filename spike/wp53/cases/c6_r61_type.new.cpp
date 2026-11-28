#include "../common.h"
namespace { int s_count = 0; }
void f() { auto l = [](int v) -> Obj& { sink(v); static Obj x = make(1); x.hits += v * 3; return x; }; Obj& r = l(s_count); ++s_count; sink(r.hits + s_count); }
