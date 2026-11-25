// WP-46 (F-41.2): with cl's legacy lambda processor every lambda of this file is named `<lambda_<hash>>`, in no scope. The
// statics `c` live in the lambdas; a reload must keep each counter in its own range and never construct one again.
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
    return []() {
        static LlCounter c = ll_make(100000);
        c.hits += 1;
        return c.id + c.hits;
    }();
}

int ll_b()
{
    return []() {
        static LlCounter c = ll_make(200000);
        c.hits += 1;
        return c.id + c.hits;
    }();
}

int ll_c()
{
    return []() {
        static LlCounter c = ll_make(300000);
        c.hits += 1;
        return c.id + c.hits;
    }();
}

int ll_made()
{
    return s_made;
}

int ll_mark()
{
    return 1;
}
