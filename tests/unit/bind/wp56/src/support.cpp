// WP-53 unit-test inputs (tests/unit/bind/wp53, built by build.sh; not built by CMake): main and the helpers of the cases.
#include "common.h"
Obj make(int id) { Obj o; o.v = id; o.hits = 0; return o; }
volatile int g_sink;
void sink(int v) { g_sink = v; }
extern void f();
int main() { f(); return 0; }
#if defined(SPIKE_NOCRT)  // Linux pre-check: lld-link without a CRT
extern "C" {
int _tls_index = 0;
__declspec(thread) int _Init_thread_epoch = 0;
void _Init_thread_header(int*) {}
void _Init_thread_footer(int*) {}
}
#endif
