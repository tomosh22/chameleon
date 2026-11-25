#pragma once

// Fixture `planner_pairing` (WP-40, ADR-0043, R-48): one function per bind-planner defect, each in the translation unit
// its edit recompiles. Every value printed is an id plus a count, so the leading digits say WHICH object a value comes
// from, and a re-initialised object restarts just above its id.

struct PpObj {
    int id;
    int hits;  // how often it was fetched: state that must survive a reload
};
// Counts every PpObj ever constructed (pp_made()): an object constructed twice shows here.
PpObj pp_make(int id);
int pp_made();

// scope.cpp (R-48 item 3). The edit adds a FILE-SCOPE `static int helper(int)` next to this member; the old member must
// keep its own code (main.cpp calls it: `member=`), the new static function is used by pp_scope_local() (`local=`).
struct PpScope {
    static int helper(int v);
};
int pp_scope_local();

// guards.cpp (R-48 item 2, the guard rule). The edit gives pp_fresh() a NEW function-local static with a dynamic
// initialiser (and so a guard): after the reload it must be constructed (`fresh=` its id 600000 plus its count).
int pp_fresh();
int pp_guards_tick();  // a file-scope static of guards.cpp that teaches the section layout (`gt=`)

// tsi.cpp (R-48 item 7), compiled with /Zc:threadSafeInit- (cl guards each block with a bitmask `$S1`): two statics of
// one name in two blocks of one function (`s0=`, `s1=`, ids 100000 and 150000).
int pp_tsi(int idx);
int pp_tsi_mark();

// internal.cpp (R-48 item 11): the same in a function with internal linkage, whose records cl writes under a
// qualified name (`i0=`, `i1=`, ids 200000 and 250000).
int pp_internal(int idx);
int pp_internal_mark();

// swap.cpp (R-48 item 5): two lambdas with a static each, one taking no argument and one an int (`la=` 300000,
// `lb=` 350000). The edit swaps the two expressions: the compiler gives each the other's number. The reload is refused.
void pp_swap_step();
int pp_swap_a();
int pp_swap_b();

// twins.cpp (R-48 item 5, rule (d)): two lambdas with the same call operator and a static each (`ta=` 400000, `tb=`
// 450000 by 2). The edit swaps them; the code of the call operators tells them apart and the statics keep their values.
void pp_twins_step();
int pp_twins_a();
int pp_twins_b();

// sibling.cpp (R-48 item 8, refusal kept): two `names` in two blocks of one function (`n0=` 500000, `n1=` 550000); the
// edit adds a new guarded static to the first block. Which guard is whose cannot be told: refused (CML3207 / CML3600).
int pp_sibling(int idx);
