#pragma once

// The shape of Godot's SNAME macro (core/string/string_name.h):
//     #define SNAME(m_arg) ([]() -> const StringName & { static StringName sname = StringName(m_arg, true); return sname; })()
// Every use is a lambda with a function-local static called `sname` (and, because its initialiser is dynamic, a guard
// variable `$TSS0`). A function that uses the macro twice has two statics with the same name, told apart only by the
// lambda they live in (WP-37, ADR-0042).

struct LsName {
    int id;    // what the lambda was created with
    int hits;  // how often it was fetched; state that must survive a reload
};

// Counts how many LsName were ever constructed (ls_constructions()): a static that is initialised again shows here.
LsName ls_make_name(int id);

#define LS_NAME(m_id) ([]() -> LsName& { static LsName sname = ls_make_name(m_id); return sname; })()

int ls_constructions();
// Three LS_NAME uses in one function. Returns the `mark` the scenario patches (1 -> 2).
int ls_step();
// The current value (id + hits) of each of the three.
int ls_a();
int ls_b();
int ls_c();
// Two overloads with one LS_NAME each: their lambdas have the same display name.
int ls_over(int k);
int ls_over(double k);
