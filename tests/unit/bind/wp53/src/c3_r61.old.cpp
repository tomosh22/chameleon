#include "common.h"
namespace { int s_count = 0; }
void f() { static Obj x = make(1); auto l = [](int v) { sink(v); return v * 3; }; x.hits += l(s_count); ++s_count; sink(x.hits + s_count); }
