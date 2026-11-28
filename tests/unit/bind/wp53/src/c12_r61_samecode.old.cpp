#include "common.h"
namespace { int s_count = 0; }
// R-61 where the edit leaves the machine code of f as it was: f's own x moved into a lambda that only returns it.
void f() { auto l = []() -> int { return 3; }; static Obj x = make(1); x.hits += l(); ++s_count; sink(x.hits + s_count); }
