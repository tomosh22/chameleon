#include "../common.h"
namespace { int s_count = 0; }
void f() { static Obj x = make(1); Obj& a = []() -> Obj& { static Obj x = make(2); return x; }(); a.hits += x.v; ++s_count; sink(a.hits + s_count); }
