#include "h15.h"
static int s_count = 0;
volatile int g_out;
void f(int* a, int n) { static int rx = 0; ++s_count; rx += a[0]; g_out = h15_h() + rx + s_count; (void)n; }
void g(void) { g_out = 1; }
