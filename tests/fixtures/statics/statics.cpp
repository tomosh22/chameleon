#include "statics.h"

#define CML_NOINLINE __declspec(noinline)

namespace {
int s_calls = 0;                              // file-static data (.bss)
}

struct Accum {
    static int total;                         // static member data (.data)
};
int Accum::total = 0;

static const int k_weights[3] = {1, 10, 100}; // const data (.rdata)

struct Guarded {
    int base;
    explicit Guarded(int b) : base(b) {}
};

static CML_NOINLINE int make_base()
{
    return 3;
}

int statics_value()
{
    static Guarded g(make_base());            // function-local static with a thread-safe guard
    ++s_calls;
    Accum::total += k_weights[s_calls % 3];
    return g.base;
}

int statics_calls()
{
    return s_calls;
}

int statics_total()
{
    return Accum::total;
}
