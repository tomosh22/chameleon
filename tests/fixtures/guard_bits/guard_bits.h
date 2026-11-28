#pragma once

// Fixture `guard_bits` (WP-63, R-69, ADR-0047): an inline function keeps its constant-initialised `static int calls = 0;`
// and gains a dynamically initialised static, without thread-safe statics (bits_nts.cpp, /Zc:threadSafeInit-: one bitmask
// `??_B` per inline function) and with them (bits_ts.cpp: an external `?$TSS<n>` per static).

struct GbName {
    int id;
    int hits;
};
GbName gb_make(int id);  // make.cpp: counts the statics constructed (gb_made)
int gb_made();

void gb_nts_step();
int gb_nts_value();  // 100000 + the calls of gb_nts_counter()
int gb_nts_extra();  // id + hits of its new static (0 until the edit)
int gb_nts_mark();

void gb_ts_step();
int gb_ts_value();  // 200000 + the calls of gb_ts_counter()
int gb_ts_extra();
int gb_ts_mark();
