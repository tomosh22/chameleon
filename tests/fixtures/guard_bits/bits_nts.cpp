// Fixture `guard_bits` (WP-63, R-69, ADR-0047): compiled with /Zc:threadSafeInit- (clang-cl's -fno-threadsafe-statics;
// CMakeLists.txt). gb_nts_counter() is inline and keeps its constant-initialised `calls`, which tests no guard. Edit nts_1
// adds a dynamically initialised static `extra`: both compilers then guard the function's statics with ONE external bitmask,
// `??_B?1??gb_nts_counter@@YAHXZ@51`, whose name ties it to calls (bound to the running one) and to extra (new). WP-59
// refused that (CML3600: the two cannot be told apart by their symbols); WP-63 reads from the code that only extra's bit is
// claimed, so the bitmask is new, extra is constructed once and calls keeps counting (docs/wp/WP-63.md).
//
// noinline keeps gb_nts_counter() a function of its own, which the reload hooks.
#include "guard_bits.h"

int g_gb_nts = 0;  // external on purpose (an internal int could be folded into the code)
int g_gb_nts_extra = 0;

__declspec(noinline) inline int gb_nts_counter()
{
    static int calls = 0;
    calls += 1;
    return 100000 + calls;
}

void gb_nts_step()
{
    g_gb_nts = gb_nts_counter();
}

int gb_nts_value()
{
    return g_gb_nts;
}

int gb_nts_extra()
{
    return g_gb_nts_extra;
}

int gb_nts_mark()
{
    return 1;
}
