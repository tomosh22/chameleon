#include "common.h"
// counter() gains two dynamic statics, one with a destructor (/EHsc): a (bit 1) and d (bit 2, registered with atexit). The
// EH funclets clear their bits again when a constructor throws (`and byte ptr [bitmask], ~bit`).
__declspec(noinline) inline int counter() {
  static int calls = 0;
  static Obj a = make(100);
  static D d(5);
  calls += 1;
  a.hits += 1;
  return calls + a.v + a.hits + d.v;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
