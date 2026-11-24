#include "names.h"

int g_ls_last[3] = {0, 0, 0};  // external on purpose: clang-cl at /O2 splits an internal array into pieces `name.N`
namespace {
int s_constructions = 0;
}

LsName ls_make_name(int id)
{
    ++s_constructions;
    LsName n;
    n.id = id;
    n.hits = 0;
    return n;
}

int ls_constructions()
{
    return s_constructions;
}

// Three lambdas in ONE function, each with a function-local static `sname`.
int ls_step()
{
    LsName& a = LS_NAME(100000);
    LsName& b = LS_NAME(200000);
    LsName& c = LS_NAME(300000);
    a.hits += 1;
    b.hits += 2;
    c.hits += 3;
    g_ls_last[0] = a.id + a.hits;
    g_ls_last[1] = b.id + b.hits;
    g_ls_last[2] = c.id + c.hits;
    return 1;
}

int ls_a()
{
    return g_ls_last[0];
}

int ls_b()
{
    return g_ls_last[1];
}

int ls_c()
{
    return g_ls_last[2];
}

// The lambdas of two overloads: both are "ls_over::<lambda>::operator()" by display name.
int ls_over(int k)
{
    LsName& n = LS_NAME(400000);
    n.hits += 1;
    return n.id + n.hits + k;
}

int ls_over(double k)
{
    LsName& n = LS_NAME(500000);
    n.hits += 1;
    return n.id + n.hits + static_cast<int>(k);
}
