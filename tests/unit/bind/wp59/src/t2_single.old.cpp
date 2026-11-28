#include "common.h"
// counter() has one static, whose block has one guard ?$TSS0@?1??counter@@YAHXZ@4HA (a Meyers singleton).
__declspec(noinline) inline Obj& counter() {
  static Obj a = make(100);
  return a;
}
int mark() { return 1; }
void f() { Obj& a = counter(); a.hits += 1; sink(a.v + a.hits + mark()); }
