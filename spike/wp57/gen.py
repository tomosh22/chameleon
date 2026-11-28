import os, sys
D = sys.argv[1]
SORT = '#include <algorithm>\n'
HEAD = '#include "common.h"\nnamespace { int s_count = 0; }\nFlag s_flag;\n'
G1 = 'void g() { sink(1); }\n'
G2 = 'void g() { sink(2); }\n'

F_SORT = 'void f(int* a, int n) { static Obj x = make(1); std::sort(a, a + n, [](int l, int r) { return l > r; }); x.hits += a[0] + s_count; ++s_count; sink(x.hits); }\n'
F_SORT_EDIT = 'void f(int* a, int n) { static Obj x = make(1); std::sort(a, a + n, [](int l, int r) { return l > r; }); x.hits += a[n - 1] + s_count; ++s_count; sink(x.hits); }\n'
F_ONCE = 'void f(int* a, int n) { static Obj x = make(1); once(s_flag, [&] { a[0] = n; sink(n); }); x.hits += a[0] + s_count; ++s_count; sink(x.hits); }\n'
F_ONCE_EDIT = 'void f(int* a, int n) { static Obj x = make(1); once(s_flag, [&] { a[0] = n; sink(n); }); x.hits += a[1] + s_count; ++s_count; sink(x.hits); }\n'
# R-54: the lambda's static x, inlined into f; the edit deletes the lambda and declares x in f.
F_R54_OLD = 'void f(int* a, int n) { auto l = [](int v) { static Obj x = make(1); x.hits += v; return x.hits; }; sink(l(a[0]) + s_count); ++s_count; (void)n; }\n'
F_R54_NEW = 'void f(int* a, int n) { static Obj x = make(1); x.hits += a[0]; sink(x.hits + s_count); ++s_count; (void)n; }\n'
# R-54, the lambda kept (without its static), x moved into f.
F_R54_KEEP = 'void f(int* a, int n) { static Obj x = make(1); auto l = [](Obj& o, int v) { o.hits += v; return o.hits; }; sink(l(x, a[0]) + s_count); ++s_count; (void)n; }\n'
# R-54 with the old machine code: a constant-initialised static moved out of the lambda.
F_R54_SAME_OLD = 'void f(int* a, int n) { auto l = [] { static int x = 5; return ++x; }; sink(l() + a[0]); (void)n; }\n'
F_R54_SAME_NEW = 'void f(int* a, int n) { static int x = 5; sink(++x + a[0]); (void)n; }\n'
# f's own x next to a sort lambda and an inlined lambda with its own static y (WP-53's listing too).
F_BOTH = 'void f(int* a, int n) { static Obj x = make(1); std::sort(a, a + n, [](int l, int r) { return l > r; }); auto c = [] { static Obj y = make(2); return ++y.hits; }; x.hits += a[0] + c() + s_count; ++s_count; sink(x.hits); }\n'
# The benign twin of r54_keep: f's own x and a lambda without statics; f edited.
F_BENIGN_OLD = 'void f(int* a, int n) { static Obj x = make(1); auto l = [](Obj& o, int v) { o.hits += v; return o.hits; }; sink(l(x, a[0]) + s_count); ++s_count; (void)n; }\n'
F_BENIGN_NEW = 'void f(int* a, int n) { static Obj x = make(1); auto l = [](Obj& o, int v) { o.hits += v; return o.hits; }; sink(l(x, a[1]) + s_count); ++s_count; (void)n; }\n'

F_R54_KEEP_SIG = 'void f(int* a, int n) { static Obj x = make(1); auto l = [](int v) { return v * 2; }; x.hits += l(a[0]); sink(x.hits + s_count); ++s_count; (void)n; }\n'
F_R54L_OLD = 'void f(int* a, int n) { ++s_count; auto l = [](int v) { static Obj x = make(1); x.hits += v; return x.hits; }; sink(l(a[0]) + s_count); (void)n; }\n'
F_R54L_NEW = 'void f(int* a, int n) { ++s_count; static Obj x = make(1); x.hits += a[0]; sink(x.hits + s_count); (void)n; }\n'
cases = {
    's12_r54_layout': (HEAD + F_R54L_OLD + G1, HEAD + F_R54L_NEW + G1),
    's10_r54_keep_sig': (HEAD + F_R54_OLD + G1, HEAD + F_R54_KEEP_SIG + G1),
    's11_benign_sig': (HEAD + F_R54_KEEP_SIG + G1, HEAD + F_R54_KEEP_SIG.replace('a[0]', 'a[1]') + G1),
    's1_sort_other': (SORT + HEAD + F_SORT + G1, SORT + HEAD + F_SORT + G2),
    's2_sort_edit': (SORT + HEAD + F_SORT + G1, SORT + HEAD + F_SORT_EDIT + G1),
    's3_once_other': (HEAD + F_ONCE + G1, HEAD + F_ONCE + G2),
    's4_once_edit': (HEAD + F_ONCE + G1, HEAD + F_ONCE_EDIT + G1),
    's5_r54': (HEAD + F_R54_OLD + G1, HEAD + F_R54_NEW + G1),
    's6_r54_keep': (HEAD + F_R54_OLD + G1, HEAD + F_R54_KEEP + G1),
    's7_r54_samecode': (HEAD + F_R54_SAME_OLD + G1, HEAD + F_R54_SAME_NEW + G1),
    's8_both_other': (SORT + HEAD + F_BOTH + G1, SORT + HEAD + F_BOTH + G2),
    's9_benign_edit': (HEAD + F_BENIGN_OLD + G1, HEAD + F_BENIGN_NEW + G1),
}
for name, (old, new) in cases.items():
    open(os.path.join(D, name + '.old.cpp'), 'w').write(old)
    open(os.path.join(D, name + '.new.cpp'), 'w').write(new)
print(' '.join(cases))
