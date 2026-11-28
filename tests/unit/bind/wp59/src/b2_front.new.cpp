#include "common.h"
// counter() is inline: clang-cl -fno-threadsafe-statics guards its statics a and b with ONE bitmask, ??_B?1??counter@@YAHXZ@51
// (external, bits 0 and 1). noinline keeps it a function of its own, which is what proves the bits.
__declspec(noinline) inline int counter() {
  static Obj z = make(50);
  static Obj a = make(100);
  static Obj b = make(200);
  a.hits += 1;
  z.hits += 1;
  b.hits += 2;
  return a.v + a.hits + b.v + b.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
