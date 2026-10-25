// Object-format coverage for the COFF/PE readers. Each construct is labelled with what it
// produces in extras.obj. Keep it deterministic: extras_value() must return the same number on
// every call within a run.
#include "extras.h"

#include <windows.h>

#include <stdexcept>

namespace {

// Static (internal) data in .data and a const table in .rdata.
int s_counter = 5;
const int k_table[4] = {2, 3, 5, 7};

// Dynamic initialiser: a .CRT$XCU entry pointing at a ??__E... function.
int make_dynamic() { return static_cast<int>(GetCurrentProcessId() % 7u) + 1; }
int s_dynamic = make_dynamic();

} // namespace

// COMDAT functions: an inline function and a template instantiation (selection = ANY).
inline int inline_twice(int v) { return v * 2; }
template <typename T> T add_one(T v) { return v + 1; }

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
static int guarded_length(const char* text)
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

    int v = s_counter + k_table[s_counter % 4] + (s_dynamic > 0 ? 1 : 0);
    v += inline_twice(3) + add_one(4) + static_cast<int>(add_one<long long>(5));
    v += fixture_optional_hook() + fixture_weak_symbol() + imported;
    v += guarded_length("extras-string-literal");
    return v;
}
