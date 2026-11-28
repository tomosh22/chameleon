#include "common.h"
namespace { int s_count = 0; }
Flag s_flag;
void f(int* a, int n) { auto l = [] { static int x = 5; return ++x; }; sink(l() + a[0]); (void)n; }
void g() { sink(1); }
