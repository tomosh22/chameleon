#include "../common.h"
namespace { int s_count = 0; }
void f() { Obj& r = []() -> Obj& { static Obj x = make(1); return x; }(); r.hits += 3; ++s_count; sink(r.hits + s_count); }
