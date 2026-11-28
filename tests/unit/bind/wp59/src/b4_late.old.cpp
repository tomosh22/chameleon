#include "common.h"
// counter()'s static a is constant-initialised: no bitmask in the old image.
__declspec(noinline) inline int counter() {
  static Obj a = {100, 0};
  a.hits += 1;
  return a.v + a.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
