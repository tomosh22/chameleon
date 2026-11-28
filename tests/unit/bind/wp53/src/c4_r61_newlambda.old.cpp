#include "common.h"
namespace { int s_count = 0; }
void f() { static Obj x = make(1); x.hits += 3; ++s_count; sink(x.hits + s_count); }
