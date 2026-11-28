#include "../common.h"
namespace { int s_count = 0; }
void f() { Obj& a = []() -> Obj& { static Obj x = make(2); return x; }(); a.hits += 1; ++s_count; sink(a.hits + s_count); }
