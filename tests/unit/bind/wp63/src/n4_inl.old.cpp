#include "common.h"
// counter() is inline (not noinline): inlined into f(). Its only static is constant-initialised.
inline int counter() {
  static int calls = 0;
  calls += 1;
  return calls;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); sink(counter()); }
