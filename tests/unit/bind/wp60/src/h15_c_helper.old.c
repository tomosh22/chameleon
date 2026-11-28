#include "h15.h"
static int s_count = 0;
volatile int g_out;
void f(int* a, int n) { ++s_count; g_out = h15_h() + a[0] + s_count; (void)n; }
void g(void) { g_out = 1; }
