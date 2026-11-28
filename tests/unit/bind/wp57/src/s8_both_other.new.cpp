#include <algorithm>
#include "common.h"
namespace { int s_count = 0; }
Flag s_flag;
void f(int* a, int n) { static Obj x = make(1); std::sort(a, a + n, [](int l, int r) { return l > r; }); auto c = [] { static Obj y = make(2); return ++y.hits; }; x.hits += a[0] + c() + s_count; ++s_count; sink(x.hits); }
void g() { sink(2); }
