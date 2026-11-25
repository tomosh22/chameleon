// R-48 item 1 (the headline of WP-40): compiled with /O2 /Ob2, clang-cl inlines the call operators of these lambdas into
// o2_step(), emits no procedure for them and records their statics `sname` in o2_step() itself. Until WP-40 the planner
// found no record of `sname` in a call operator, gave the patch new statics, and bound their guards to the OLD guards by
// the section layout: the patched code used objects that were never constructed. Now the statics are looked for in the
// calling procedure and placed by the section layout (s_calls teaches it), or the reload is refused.
#include "o2.h"

namespace {
int s_made = 0;
int s_calls = 0;  // file-scope state in the section of the statics
}  // namespace
int g_o2_values[3] = {0, 0, 0};  // external on purpose: clang-cl at /O2 splits an internal array (that is parts.cpp)

O2Name o2_make(int id)
{
    ++s_made;
    O2Name n;
    n.id = id;
    n.hits = 0;
    return n;
}

int o2_made()
{
    return s_made;
}

#define O2_NAME(m_id) ([]() -> O2Name& { static O2Name sname = o2_make(m_id); return sname; })()

void o2_step()
{
    O2Name& a = O2_NAME(100000);
    O2Name& b = O2_NAME(200000);
    O2Name& c = O2_NAME(300000);
    a.hits += 1;
    b.hits += 2;
    c.hits += 3;
    ++s_calls;
    g_o2_values[0] = a.id + a.hits;
    g_o2_values[1] = b.id + b.hits;
    g_o2_values[2] = c.id + c.hits;
}

int o2_value(int k)
{
    return g_o2_values[k] + (s_calls > 0 ? 0 : 1);
}

int o2_mark()
{
    return 1;
}
