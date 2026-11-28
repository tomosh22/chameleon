#include "common.h"
// counter() gains two statics and their bitmask: all new.
__declspec(noinline) inline int counter() {
  static Obj a = make(100);
  static Obj b = make(200);
  a.hits += 1;
  b.hits += 2;
  return a.v + a.hits + b.v + b.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
