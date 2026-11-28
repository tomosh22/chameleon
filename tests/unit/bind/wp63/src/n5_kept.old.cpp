#include "common.h"
// counter() has a constant-initialised calls and a dynamic a (bit 1): the old image has the bitmask.
__declspec(noinline) inline int counter() {
  static int calls = 0;
  static Obj a = make(100);
  calls += 1;
  a.hits += 1;
  return calls + a.v + a.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
