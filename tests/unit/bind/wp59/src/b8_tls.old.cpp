#include "common.h"
// counter() has a thread_local between its two statics: a and b share the bitmask ??_B?1??counter@@YAHXZ@51 (bits 0 and
// 2), the thread_local t has a guard of its own kind, ??__J?1??counter@@YAHXZ@51, in .tls$ (WP-24's).
__declspec(noinline) inline int counter() {
  static Obj a = make(100);
  thread_local Obj t = make(300);
  static Obj b = make(200);
  a.hits += 1;
  t.hits += 1;
  b.hits += 2;
  return a.v + a.hits + t.v + b.v + b.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
