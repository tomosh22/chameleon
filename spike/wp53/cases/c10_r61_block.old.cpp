#include "../common.h"
namespace { int s_count = 0; }
void f() { if (s_count >= 0) { static Obj x = make(1); x.hits += 1; sink(x.hits); } auto l = [](int v) { sink(v); return v * 3; }; ++s_count; sink(l(s_count)); }
