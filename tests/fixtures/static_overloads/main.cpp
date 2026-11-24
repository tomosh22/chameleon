// Fixture `static_overloads` (WP-37): prints
//   TICK n=<k> af=<a_float()> av=<a_vec()> aw=<a_weight()> bf=<b_float()> bv=<b_vec()> acalls=<n> bcalls=<n>
#include <cstdio>

#include "fixture_runtime.h"
#include "tus.h"

int g_so_zero = 0;

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        const int af = a_float();
        const int bf = b_float();
        std::printf("TICK n=%d af=%d av=%d aw=%d bf=%d bv=%d acalls=%d bcalls=%d\n", n, af, a_vec(), a_weight(), bf, b_vec(), a_calls(), b_calls());
    });
}
