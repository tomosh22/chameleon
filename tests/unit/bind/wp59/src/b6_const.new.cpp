#include "common.h"
// counter() gains a dynamically initialised static a (bit 1; calls, still constant-initialised, is static number 0).
__declspec(noinline) inline int counter() {
  static int calls = 0;
  static Obj a = make(100);
  calls += 1;
  a.hits += 1;
  return calls + a.v + a.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
