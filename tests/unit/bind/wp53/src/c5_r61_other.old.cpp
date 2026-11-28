#include "common.h"
namespace { int s_count = 0; }
void f() { static Obj x = make(1); Obj& y = []() -> Obj& { static Obj y = make(4); return y; }(); y.hits += x.v; ++s_count; sink(y.hits + s_count); }
