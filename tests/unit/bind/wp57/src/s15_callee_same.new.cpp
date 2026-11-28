#include "common.h"
namespace { int s_count = 0; }
Flag s_flag;
template <class F> __declspec(noinline) static int apply(int* a, int n, F fn) { int t = 0; for (int i = 0; i < n; ++i) t += fn(a[i]); return t; }
void f(int* a, int n) { static Obj x = make(1); once(s_flag, [&] { a[0] = n; }); x.hits += apply(a, n, [](int v) { return v * 2; }) + s_count; ++s_count; sink(x.hits); }
void g() { sink(2); }
