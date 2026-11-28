// Fixture `guard_bits` (WP-63, R-69, ADR-0047): thread-safe statics (the default). gb_ts_counter() is inline and keeps its
// constant-initialised `calls`, which has no guard. Edit ts_2 adds a dynamically initialised static `extra` in the same
// block: its external guard (clang-cl: `?$TSS1@?1??gb_ts_counter@@YAHXZ@4HA`, numbered by extra's place among the
// function's statics; cl numbers its guarded statics only) is tied by its name to calls (bound) and to extra (new). WP-59
// refused that (CML3600); WP-63 reads from the code that the guard's _Init_thread_header .. _Init_thread_footer bracket
// initialises extra only, so the guard is new and calls keeps counting (docs/wp/WP-63.md).
#include "guard_bits.h"

int g_gb_ts = 0;
int g_gb_ts_extra = 0;

__declspec(noinline) inline int gb_ts_counter()
{
    static int calls = 0;
    calls += 1;
    return 200000 + calls;
}

void gb_ts_step()
{
    g_gb_ts = gb_ts_counter();
}

int gb_ts_value()
{
    return g_gb_ts;
}

int gb_ts_extra()
{
    return g_gb_ts_extra;
}

int gb_ts_mark()
{
    return 1;
}
