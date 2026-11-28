#include "common.h"
namespace { int s_count = 0; }
void f() { static int x = 5; auto l = [](int v) { sink(v); return v * 3; }; x += l(s_count); ++s_count; sink(x + s_count); }
