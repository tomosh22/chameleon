#pragma once

// Shared state of fixture `crash_reexecute` (WP-23). Owned by main.cpp, never patched.
extern int g_slot;           // a valid int the normal frames point step() / opt_step() at (7)
extern int g_seen_k;         // the `k` the last step() call saw
extern double g_seen_scale;  // the `scale` the last step() call saw

// step.cpp, built WITHOUT optimisation in every configuration. Dereferences `p`, which main.cpp makes null once on purpose.
// Returns the value read through `p` + k + scale * 1000 (truncated). The patch (edits/fix_step.edit) survives a null `p` and adds 100000.
int step(int* p, int k, double scale);

// opt.cpp, built WITH optimisation (/O2). Dereferences `p` the same way; re-execute must not be offered for it.
int opt_step(int* p, int k);
