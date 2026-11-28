#include "common.h"
// As b6, but calls is now dynamically initialised too: a fresh bitmask would run its initialiser over the live calls.
__declspec(noinline) inline int counter() {
  static int calls = make_int(0);
  static Obj a = make(100);
  calls += 1;
  a.hits += 1;
  return calls + a.v + a.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
