// WP-46 (F-41.2): with cl's legacy lambda processor every lambda of this file is named `<lambda_<hash>>`, in no scope. The
// statics `c` live in the lambdas; a reload must keep each counter in its own range and never construct one again. Each
// lambda is a named local of its function and captures its id: the planner pairs an edited (renamed) lambda only with the
// evidence that it is the old one (the same enclosing function, read from the local's type, the same capture, the same
// statics and call operator).
#include "legacy.h"

namespace {
int s_made = 0;
}  // namespace

struct LlCounter {
    int id;
    int hits;
};

static LlCounter ll_make(int id)
{
    ++s_made;
    LlCounter c;
    c.id = id;
    c.hits = 0;
    return c;
}

int ll_a()
{
    int id = 100000;
    auto lambda = [id]() {
        static LlCounter c = ll_make(id);
        c.hits += 1;
        return c.id + c.hits;
    };
    return lambda();
}

int ll_b()
{
    int id = 200000;
    auto lambda = [id]() {
        static LlCounter c = ll_make(id);
        c.hits += 1;
        return c.id + c.hits;
    };
    return lambda();
}

int ll_c()
{
    int id = 300000;
    auto lambda = [id]() {
        static LlCounter c = ll_make(id);
        c.hits += 1;
        return c.id + c.hits;
    };
    return lambda();
}

int ll_made()
{
    return s_made;
}

int ll_mark()
{
    return 1;
}
