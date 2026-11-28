// WP-59 (R-67, ADR-0046): the statics of an inline function without thread-safe initialisation (this file is compiled
// with /Zc:threadSafeInit-, clang-cl's -fno-threadsafe-statics; CMakeLists.txt). clang-cl guards both statics of
// o2_bits_counter() with ONE external bitmask, `??_B?1??o2_bits_counter@@YAHXZ@51` (bit 0 `first`, bit 1 `second`), in a
// COMDAT of its own; an internal-linkage function would get `?$S1@...@4IA` instead. Until WP-59 the planner did not know
// the name as a guard and bound it to the old bitmask as ordinary data, whatever an edit did to the function's statics.
// Now it is a guard: kept with its statics while o2_bits_counter() is unchanged (an edit of o2_bits_mark() reloads, and
// `bits` / `bsec` keep counting, `bmade` stays 2), refused when the statics change (docs/wp/WP-59.md).
//
// noinline keeps o2_bits_counter() a function of its own: its unchanged code is what proves the bits (prove_bitmask_guards).
#include "o2.h"

#if defined(_MSC_VER)
#define O2_BITS_NOINLINE __declspec(noinline)
#else
#define O2_BITS_NOINLINE
#endif

namespace {
int s_bits_made = 0;
int s_bits_calls = 0;
}  // namespace
int g_o2_bits = 0;  // external on purpose (an internal int could be folded into the code)
int g_o2_bsec = 0;

O2Name o2_bits_make(int id)
{
    ++s_bits_made;
    O2Name n;
    n.id = id;
    n.hits = 0;
    return n;
}

O2_BITS_NOINLINE inline int o2_bits_counter()
{
    static O2Name first = o2_bits_make(800000);
    static O2Name second = o2_bits_make(900000);
    first.hits += 1;
    second.hits += 1;
    g_o2_bsec = second.id + second.hits;
    return first.id + first.hits;
}

void o2_bits_step()
{
    g_o2_bits = o2_bits_counter();
    ++s_bits_calls;
}

int o2_bits_value()
{
    return g_o2_bits + (s_bits_calls > 0 ? 0 : 1);
}

int o2_bits_second()
{
    return g_o2_bsec;
}

int o2_bits_made()
{
    return s_bits_made;
}

int o2_bits_mark()
{
    return 1;
}
