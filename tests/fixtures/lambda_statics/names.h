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

// What Godot's SceneTree::get_argument_options() keeps in its two `static const Vector<StringName> names`: a constant
// object with a dynamic initialiser. (`hits` is mutable so that the scenario can see the object's state.)
struct LsList {
    int id;
    mutable int hits;
};
LsList ls_make_list(int id);  // counted by ls_constructions() as well

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

// --- the rest of what Godot's scene/main/scene_tree.cpp has (corpus run 45438434773) ---
// Member functions whose LS_NAME sits in a late block, as SceneTree::_flush_scene_change() and ::process_timers(). cl
// numbers the blocks of a function and writes the number into the decorated name of everything declared there: one
// digit up to block 10 ("?1?"), letters from block 11 on ("?L@?"). The planner's name reader has to read both.
class LsTree {
public:
    int flush(int k);            // id 600000, one LS_NAME after many blocks
    int timers(int k, bool on);  // id 700000, one LS_NAME inside nested blocks, after many more
    // Two statics of ONE name and type in two blocks of one function, no lambda (ids 1000000 and 1100000):
    //     if (idx == 0) { static const LsList names = ...; } else if (idx == 1) { static const LsList names = ...; }
    // Nothing but their place in the function tells them apart (ADR-0042, amended; CML3570 says so on every reload).
    int options(int idx) const;
};
// A function of an anonymous namespace with two LS_NAME (ids 800000 and 900000), in a translation unit of its own
// (anon.cpp) that no scenario edits: `x` is the first one's value, `y` the second's.
void ls_anon_step();
int ls_anon_x();
int ls_anon_y();
