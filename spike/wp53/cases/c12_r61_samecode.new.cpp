#include "../common.h"
namespace { int s_count = 0; }
// R-61 where the edit leaves the machine code of f as it was: f's own x moved into a lambda that only returns it.
void f() { auto l = []() -> Obj& { static Obj x = make(1); return x; }; Obj& x = l(); x.hits += 3; ++s_count; sink(x.hits + s_count); }
