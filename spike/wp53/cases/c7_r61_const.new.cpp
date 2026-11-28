#include "../common.h"
namespace { int s_count = 0; }
void f() { auto l = [](int v) -> int& { sink(v); static int x = 5; x += v * 3; return x; }; int& r = l(s_count); ++s_count; sink(r + s_count); }
