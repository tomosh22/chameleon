#include "../common.h"
namespace { int s_count = 0; }
void f() { auto l = []() -> Obj& { static Obj y = make(4); static Obj x = make(1); y.hits += x.v; return y; }; Obj& y = l(); ++s_count; sink(y.hits + s_count); }
