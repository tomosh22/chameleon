// WP-60 unit-test inputs (tests/unit/bind/wp60, built by build.sh; not built by CMake): main and the helpers of the cases.
#include "common.h"
Obj make(int id) { Obj o; o.v = id; o.hits = 0; return o; }
volatile int g_sink;
void sink(int v) { g_sink = v; }
int (*volatile g_ext_glob)(int) = &ext_glob;  // keeps an out-of-line (global) copy of ext_glob in this compiland (h13)
#ifdef WP60_C_CASE  // a case compiled as C (h15): its functions have C linkage
extern "C" void f(int* a, int n);
extern "C" void g(void);
#else
extern void f(int* a, int n);
extern void g();
#endif
int main() { int a[4] = {3, 1, 2, 4}; f(a, 4); g(); return 0; }
extern "C" {
int _tls_index = 0;
__declspec(thread) int _Init_thread_epoch = 0;
void _Init_thread_header(int*) {}
void _Init_thread_footer(int*) {}
void* memmove(void* d, const void* s, unsigned long long n) {
    unsigned char* dd = (unsigned char*)d;
    const unsigned char* ss = (const unsigned char*)s;
    if (dd < ss) { for (unsigned long long i = 0; i < n; ++i) dd[i] = ss[i]; }
    else { for (unsigned long long i = n; i > 0; --i) dd[i - 1] = ss[i - 1]; }
    return d;
}
}
