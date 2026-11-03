// Fixture `optimised`: a /O2 /Ob2 build (the other fixtures use /Od or /Ob0). Prints
// `TICK n=<k> value=<compute_value()> small=<small_value()>`; editing small_value() in logic.cpp changes a small
// function of an optimised compiland, which Chameleon reports as CML3104 (copies inlined into other translation units
// keep the old code). The call through a volatile function pointer keeps small_value() a real function.
#include <cstdio>

#include "fixture_runtime.h"
#include "logic.h"

int main(int argc, char** argv)
{
    static int (*volatile const direct)() = &small_value;
    return cml_fixture::run(argc, argv, [](int n) {
        std::printf("TICK n=%d value=%d small=%d\n", n, compute_value(), direct());
    });
}
