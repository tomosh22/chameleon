#include "common.h"
// counter() gains a second dynamic static b (bit 2) next to the bound, dynamic a: the code shows a bound static and a new
// one under the bitmask, which stays refused (CML3600).
__declspec(noinline) inline int counter() {
  static int calls = 0;
  static Obj a = make(100);
  static Obj b = make(200);
  calls += 1;
  a.hits += 1;
  b.hits += 1;
  return calls + a.v + a.hits + b.v + b.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
