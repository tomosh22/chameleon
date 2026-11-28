#include "common.h"
// counter() gains a dynamically initialised static a; calls stays constant-initialised. Thread-safe statics: a's guard is
// ?$TSS1@?1??counter@@YAHXZ@4HA (number 1: calls is static number 0), in calls' block.
__declspec(noinline) inline int counter() {
  static int calls = 0;
  static Obj a = make(100);
  calls += 1;
  a.hits += 1;
  return calls + a.v + a.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
