struct Obj { int v; int hits; };
Obj make(int id);
void sink(int);
struct Flag { int done; };
// A stand-in for std::call_once: the lambda's code is inlined into the caller.
template <class F> inline void once(Flag& fl, F&& fn) {
    if (!fl.done) {
        fn();
        fl.done = 1;
    }
}
