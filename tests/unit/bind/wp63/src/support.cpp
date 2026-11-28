// WP-63 unit-test inputs (tests/unit/bind/wp63, built by build.sh; not built by CMake): main and the helpers of the cases.
#include "common.h"
Obj make(int id) { Obj o; o.v = id; o.hits = 0; return o; }
int make_int(int v) { return v + 1; }
D::D(int x) : v(x) {}
D::~D() { v = 0; }
volatile int g_sink;
void sink(int v) { g_sink = v; }
void keep(int* p) { g_sink = *p; }
extern void f();
int main() { f(); return 0; }
// lld-link without a CRT: what the thread-safe statics, the destructors and the EH code of the cases call.
extern "C" {
int _tls_index = 0;
__declspec(thread) int _Init_thread_epoch = 0;
void _Init_thread_header(int*) {}
void _Init_thread_footer(int*) {}
void _Init_thread_abort(int*) {}
int atexit(void (*)(void)) { return 0; }
int __CxxFrameHandler3() { return 0; }
int __CxxFrameHandler4() { return 0; }
int __C_specific_handler() { return 0; }
}
