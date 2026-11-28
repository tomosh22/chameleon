#include "common.h"
// As b6 (a dynamic a added next to the constant-initialised calls), but the code puts calls' address in a register
// (`lea rcx, [calls]` for keep()): a region could write calls through it, so the code does not prove calls untouched and
// the bitmask stays refused (CML3600).
__declspec(noinline) inline int counter() {
  static int calls = 0;
  static Obj a = make(100);
  calls += 1;
  keep(&calls);
  a.hits += 1;
  return calls + a.v + a.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); }
