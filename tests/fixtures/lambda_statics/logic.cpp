#include "names.h"

int g_ls_last[8] = {0, 0, 0};  // external on purpose: clang-cl at /O2 splits an internal array into pieces `name.N`
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

// --- Godot's shape: member functions, LS_NAME in block 11 or later ---------------------------------------------------
// Every `{ ... }` below is a block of its own for cl's numbering; the LS_NAME of flush() comes after ten of them, the
// one of timers() after twenty and inside a loop and two conditions. (What the blocks compute does not matter.)
int LsTree::flush(int k)
{
    int sum = 0;
    if (k > 1) { int t = k + 1; sum += t; }
    if (k > 2) { int t = k + 2; sum += t; }
    if (k > 3) { int t = k + 3; sum += t; }
    if (k > 4) { int t = k + 4; sum += t; }
    if (k > 5) { int t = k + 5; sum += t; }
    if (k > 6) { int t = k + 6; sum += t; }
    if (k > 7) { int t = k + 7; sum += t; }
    if (k > 8) { int t = k + 8; sum += t; }
    if (k > 9) { int t = k + 9; sum += t; }
    if (k > 10) { int t = k + 10; sum += t; }
    if (sum >= 0) {
        LsName& n = LS_NAME(600000);
        n.hits += 1;
        sum = n.id + n.hits;
    }
    return sum;
}

int LsTree::timers(int k, bool on)
{
    int sum = 0;
    for (int i = 0; i < 2; ++i) {
        if (k > i + 1) { int t = k + 1; sum += t; }
        if (k > i + 2) { int t = k + 2; sum += t; }
        if (k > i + 3) { int t = k + 3; sum += t; }
        if (k > i + 4) { int t = k + 4; sum += t; }
        if (k > i + 5) { int t = k + 5; sum += t; }
        if (k > i + 6) { int t = k + 6; sum += t; }
        if (k > i + 7) { int t = k + 7; sum += t; }
        if (k > i + 8) { int t = k + 8; sum += t; }
        if (k > i + 9) { int t = k + 9; sum += t; }
        if (k > i + 10) { int t = k + 10; sum += t; }
    }
    for (int i = 0; i < 2; ++i) {
        if (k > i + 11) { int t = k + 11; sum += t; }
        if (k > i + 12) { int t = k + 12; sum += t; }
        if (k > i + 13) { int t = k + 13; sum += t; }
        if (k > i + 14) { int t = k + 14; sum += t; }
        if (k > i + 15) { int t = k + 15; sum += t; }
        if (k > i + 16) { int t = k + 16; sum += t; }
        if (k > i + 17) { int t = k + 17; sum += t; }
        if (k > i + 18) { int t = k + 18; sum += t; }
        if (i == 0) {
            if (on) {
                LsName& n = LS_NAME(700000);
                n.hits += 1;
                g_ls_last[3] = n.id + n.hits;
            }
        }
    }
    return sum >= 0 ? g_ls_last[3] : 0;
}

// --- Godot's SceneTree::get_argument_options(): two statics of one name in two blocks of one function ----------------
LsList ls_make_list(int id)
{
    ++s_constructions;
    LsList l;
    l.id = id;
    l.hits = 0;
    return l;
}

int LsTree::options(int idx) const
{
    int value = 0;
    if (idx == 0) {
        static const LsList names = ls_make_list(1000000);
        names.hits += 1;
        value = names.id + names.hits;
    } else if (idx == 1) {
        static const LsList names = ls_make_list(1100000);
        names.hits += 1;
        value = names.id + names.hits;
    }
    return value;
}
