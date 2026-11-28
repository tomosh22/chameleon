#include "common.h"
namespace { int s_count = 0; }
#define N(i) ([]() -> Obj& { static Obj x = make(i); return x; })()
void f() { Obj& a = N(1); Obj& b = N(2); Obj& c = N(3); a.hits += 1; b.hits += 2; c.hits += 4; ++s_count; sink(a.hits + b.hits + c.hits + s_count); }
