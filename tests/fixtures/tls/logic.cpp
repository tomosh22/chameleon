// Fixture `tls`: the translation unit the scenario patches.
#include "logic.h"

namespace {
const int kStep = 1;
} // namespace

thread_local int t_counter = 0;  // namespace scope, external linkage

TlsValues tls_step()
{
    static thread_local int calls = 0;  // function-local static thread_local
    t_counter += kStep;
    calls += kStep;
    TlsValues v;
    v.step = kStep;
    v.ns = t_counter;
    v.fn = calls;
    return v;
}
