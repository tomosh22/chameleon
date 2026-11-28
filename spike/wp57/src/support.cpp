// WP-57 spike inputs: main and the helpers of the cases (lld-link without a CRT).
#include "common.h"
Obj make(int id) { Obj o; o.v = id; o.hits = 0; return o; }
volatile int g_sink;
void sink(int v) { g_sink = v; }
extern void f(int* a, int n);
extern void g();
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
