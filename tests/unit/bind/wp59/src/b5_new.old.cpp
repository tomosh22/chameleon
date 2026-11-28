#include "common.h"
// counter() has no statics yet.
__declspec(noinline) inline int counter() { return 7; }
int mark() { return 1; }
void f() { sink(counter() + mark()); }
