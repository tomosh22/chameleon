#include "common.h"
// counter()'s only static is constant-initialised: no guard in the old image. Its address is passed on.
__declspec(noinline) inline int counter() {
  static int calls = 0;
  calls += 1;
  keep(&calls);
  return calls;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
