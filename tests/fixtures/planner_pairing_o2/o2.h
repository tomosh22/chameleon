#pragma once

// Fixture `planner_pairing_o2` (WP-40, ADR-0043): built with /O2 /Ob2 in every cell (clang-cl inlines; cl as well, where it
// is harmless). lambdas.cpp: R-48 item 1, the statics of inlined lambdas. parts.cpp: R-48 item 4, an aggregate clang-cl
// splits.

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
