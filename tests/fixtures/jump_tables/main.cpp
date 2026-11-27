// Fixture `jump_tables`: prints `TICK n=<k> d1=<jt_dense(1)> d2=<jt_dense(2)> o=<jt_other(3)> v=<jt_value()>`.
#include <cstdio>

#include "fixture_runtime.h"
#include "jump_tables.h"

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        std::printf("TICK n=%d d1=%d d2=%d o=%d v=%d\n", n, jt_dense(1, 10), jt_dense(2, 10), jt_other(3, 10), jt_value());
    });
}
