#include "common.h"
// counter()'s static a is now dynamically initialised: the patch has a bitmask the old image does not have.
__declspec(noinline) inline int counter() {
  static Obj a = make(100);
  a.hits += 1;
  return a.v + a.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
