// Fixture `virtuals`: prints `TICK n=<k> value=<sum of areas> shapes=<count>` through virtual calls
// on objects created before any patch.
#include <cstdio>

#include "fixture_runtime.h"
#include "shapes.h"

int main(int argc, char** argv)
{
    shapes_create();
    return cml_fixture::run(argc, argv, [](int n) {
        int sum = 0;
        for (int i = 0; i < shapes_count(); ++i) {
            sum += shape_at(i)->area();
        }
        std::printf("TICK n=%d value=%d shapes=%d\n", n, sum, shapes_count());
    });
}
