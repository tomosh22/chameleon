template <class T> inline int tc(int v) {
    if constexpr (T::kHas) { static int x = 0; return x += v; }
    else { sink(v); return v * 3; }
}
