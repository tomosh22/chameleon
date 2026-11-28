#include "common.h"
// counter() is inline: its statics and their thread-safe guards ?$TSS<n>@?1??counter@@YAHXZ@4HA are external (COMDAT).
// The guard number n is the static's place among the function's statics in declaration order (a: 0, b: 1).
__declspec(noinline) inline int counter() {
  static Obj a = make(100);
  static Obj b = make(200);
  a.hits += 1;
  b.hits += 2;
  return a.v + a.hits + b.v + b.hits;
}
int mark() { return 2; }
void f() { sink(counter() + mark()); }
