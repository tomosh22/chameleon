#include "common.h"
namespace { int s_count = 0; }
Flag s_flag;
void f(int* a, int n) { ++s_count; static Obj x = make(1); x.hits += a[0]; sink(x.hits + s_count); (void)n; }
void g() { sink(1); }
