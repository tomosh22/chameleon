// Fixture `bad_builds` (WP-18): the same tiny program built in several ways that Chameleon rejects. The program does
// nothing useful; the tests run `cml_broker check` on each executable and assert the exact diagnostic id.
#include <cstdio>

int bad_build_value(int x)
{
    return x * 3 + 1;
}

int bad_build_twice(int x)
{
    return bad_build_value(x) * 2;
}

int main(int argc, char**)
{
    std::printf("bad build %d\n", bad_build_twice(argc));
    return 0;
}
