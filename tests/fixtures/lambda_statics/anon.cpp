// Fixture `lambda_statics`, second translation unit (WP-37). No scenario and no unit test edits this file: it exists so
// that the real PDB of every matrix cell shows how the compiler names the call operator of a lambda in a function of an
// anonymous namespace and its closure class (the planner compares the two; unit.bind_fixtures prints them as
// `LOCAL-OTHER lambda_statics ...`). The scenario only checks that its two statics keep counting.
#include "names.h"

int g_ls_anon[2] = {0, 0};  // external on purpose, like g_ls_last

namespace {

void anon_pair()
{
    LsName& x = LS_NAME(800000);
    LsName& y = LS_NAME(900000);
    x.hits += 1;
    y.hits += 2;
    g_ls_anon[0] = x.id + x.hits;
    g_ls_anon[1] = y.id + y.hits;
}

}  // namespace

void ls_anon_step()
{
    anon_pair();
}

int ls_anon_x()
{
    return g_ls_anon[0];
}

int ls_anon_y()
{
    return g_ls_anon[1];
}
