// Fixture `atomic_reload` (WP-47) main loop: prints `TICK n=<k> a=<a_value()> b=<b_value()> c=<c_value()> frames=<f>` every
// 100 ms. Never patched. `frames` is a thread_local of the main thread, so the old image has thread-local storage and the
// refusal of the edit is that of the NEW thread_local in tu_c.cpp (CML3400), not of the TLS machinery.
#include <cstdio>

#include "fixture_runtime.h"
#include "tus.h"

thread_local int t_frames = 0;

int main(int argc, char** argv)
{
    return cml_fixture::run(argc, argv, [](int n) {
        ++t_frames;
        std::printf("TICK n=%d a=%d b=%d c=%d frames=%d\n", n, a_value(), b_value(), c_value(), t_frames);
    });
}
