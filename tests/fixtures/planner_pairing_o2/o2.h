#pragma once

// Fixture `planner_pairing_o2` (WP-40, ADR-0043): built with /O2 /Ob2 in every cell (clang-cl inlines; cl as well, where it
// is harmless). lambdas.cpp: R-48 item 1, the statics of inlined lambdas. parts.cpp: R-48 item 4, an aggregate clang-cl
// splits. nested.cpp: R-60, the static of a nested lambda and its guard. bitmask.cpp: R-67, an inline function's statics
// without thread-safe initialisation (clang-cl's `??_B` bitmask).

struct O2Name {
    int id;
    int hits;
};
O2Name o2_make(int id);
int o2_made();

void o2_step();
int o2_value(int k);  // id + hits of the k-th lambda's static: 100000 + n, 200000 + 2n, 300000 + 3n
int o2_mark();

void o2_parts_step();
int o2_parts_sum();  // 500000 + 6n
int o2_parts_mark();

// WP-52 (R-60): nested.cpp, a static in a nested lambda (its guard spells the scope with other back-references).
void o2_nest_step();
int o2_nest_value();  // 700000 + n
int o2_nest_made();   // stays 1
int o2_nest_mark();

// WP-59 (R-67): bitmask.cpp (/Zc:threadSafeInit-), the two statics of an inline function share one `??_B` bitmask.
void o2_bits_step();
int o2_bits_value();   // 800000 + n (the first static)
int o2_bits_second();  // 900000 + n (the second static)
int o2_bits_made();    // stays 2
int o2_bits_mark();
