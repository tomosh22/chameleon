#include "common.h"
namespace { int s_count = 0; }
// c12 with the layout kept (s_count is created first on both sides): the guard is placed and the code of f is the old code.
void f() { int c = ++s_count; auto l = []() -> int { return 3; }; static Obj x = make(1); x.hits += l(); sink(x.hits + c); }
