// WP-59 unit-test inputs (tests/unit/bind/wp59, built by build.sh; not built by CMake): main and the helpers of the cases.
#include "common.h"
Obj make(int id) { Obj o; o.v = id; o.hits = 0; return o; }
volatile int g_sink;
void sink(int v) { g_sink = v; }
extern void f();
int main() { f(); return 0; }
