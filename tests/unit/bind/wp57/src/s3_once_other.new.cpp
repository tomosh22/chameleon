#include "common.h"
namespace { int s_count = 0; }
Flag s_flag;
void f(int* a, int n) { static Obj x = make(1); once(s_flag, [&] { a[0] = n; sink(n); }); x.hits += a[0] + s_count; ++s_count; sink(x.hits); }
void g() { sink(2); }
