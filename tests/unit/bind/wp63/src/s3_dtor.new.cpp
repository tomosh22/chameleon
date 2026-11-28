#include "common.h"
// counter() gains two dynamic statics, one with a destructor (/EHsc): a ($TSS1) and d ($TSS2, registered with atexit). The
// EH funclets call _Init_thread_abort with their guards when a constructor throws.
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
