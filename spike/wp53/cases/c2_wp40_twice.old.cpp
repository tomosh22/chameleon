#include "../common.h"
namespace { int s_count = 0; }
void f() { auto l = []() -> Obj& { static Obj x = make(2); return x; }; l().hits += 1; ++s_count; sink(l().hits + s_count); }
