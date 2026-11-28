#include "common.h"
// counter() gains a dynamic static a and is inlined into f() twice: both copies claim a's bit (bit 1) in f()'s code.
inline int counter() {
  static int calls = 0;
  static Obj a = make(100);
  calls += 1;
  a.hits += 1;
  return calls + a.v + a.hits;
}
int mark() { return 1; }
void f() { sink(counter() + mark()); sink(counter()); }
