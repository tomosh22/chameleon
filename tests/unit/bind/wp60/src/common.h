// WP-60 unit-test inputs (R-57): each case is one function `f(int*, int)`, a function `g` and an anonymous-namespace
// `s_count` that teaches the section layout; `make`/`sink` and `main` are in support.cpp.
struct Obj { int v; int hits; };
Obj make(int id);
void sink(int);
// An external-linkage inline function with a static (its static is one object of the program).
inline int ext_count() { static int ec = 0; return ++ec; }
struct Ext { int k; int bump() { static int ex = 0; ex += k; return ex; } };
// An external inline function whose out-of-line copy support.cpp keeps (a global procedure of another compiland).
inline int ext_glob(int v) { static int eg = 0; return eg += v; }
