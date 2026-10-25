// Object-format coverage for the COFF/PE readers. Each construct is labelled with what it
// produces in extras.obj. Keep it deterministic: extras_value() must return the same number on
// every call within a run.
//
// Every construct is kept opaque to the optimiser (CML_NOINLINE, reads through volatile) so it
// survives RelWithDebInfo (/O2 /Ob0) under both cl and clang-cl, not only Debug.
#include "extras.h"

#include <windows.h>

#include <stdexcept>

#define CML_NOINLINE __declspec(noinline)

namespace {

// Defeats constant propagation: the compiler must load the value at run time.
template <typename T> CML_NOINLINE T opaque(const T& v)
{
    return *static_cast<const volatile T*>(&v);
}

// Static (internal) data in .data and a const table in .rdata.
int s_counter = 5;
const int k_table[4] = {2, 3, 5, 7};

// Dynamic initialiser: a .CRT$XCU entry pointing at a ??__E... function.
CML_NOINLINE int make_dynamic() { return static_cast<int>(GetCurrentProcessId() % 7u) + 1; }
int s_dynamic = make_dynamic();

} // namespace

// COMDAT functions: an inline function and a template instantiation (selection = ANY).
CML_NOINLINE inline int inline_twice(int v) { return v * 2; }
template <typename T> CML_NOINLINE T add_one(T v) { return v + 1; }

// Weak externals. MSVC and clang-cl both honour /alternatename (a .drectve directive);
// clang-cl additionally emits a true IMAGE_SYM_CLASS_WEAK_EXTERNAL for __attribute__((weak)).
extern "C" int fixture_optional_hook_default() { return 0; }
extern "C" int fixture_optional_hook();
#pragma comment(linker, "/alternatename:fixture_optional_hook=fixture_optional_hook_default")
#if defined(__clang__)
extern "C" __attribute__((weak)) int fixture_weak_symbol() { return 0; }
#else
extern "C" int fixture_weak_symbol() { return 0; }
#endif

// .pdata/.xdata with a C++ EH personality (__CxxFrameHandler3/4) and a string literal.
static CML_NOINLINE int guarded_length(const char* text)
{
    try {
        if (text == nullptr) {
            throw std::runtime_error("extras: null text");
        }
        int n = 0;
        while (text[n] != '\0') {
            ++n;
        }
        return n;
    } catch (const std::exception&) {
        return -1;
    }
}

int extras_value()
{
    // __imp_ reference: GetTickCount64 is declared __declspec(dllimport) by windows.h. The value
    // is only used for a side-effect-free comparison so the result stays deterministic.
    const ULONGLONG ticks = GetTickCount64();
    const int imported = ticks == 0 ? 1 : 0;

    const int counter = opaque(s_counter);
    int v = counter + opaque(k_table[counter % 4]) + (opaque(s_dynamic) > 0 ? 1 : 0);
    v += inline_twice(opaque(3)) + add_one(opaque(4)) + static_cast<int>(add_one<long long>(opaque(5LL)));
    v += fixture_optional_hook() + fixture_weak_symbol() + imported;
    // The literal's address goes through opaque() so the null check (and its throw) stays.
    v += guarded_length(opaque<const char*>("extras-string-literal"));
    return v;
}
