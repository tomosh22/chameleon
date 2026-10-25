#pragma once

// Name helpers of the bind planner (WP-07): a small MSVC-name reader (cl and clang-cl both emit the
// Microsoft mangling), used to
//   * give every decision a compiler-independent display name ("Square::area", "statics_value::g"),
//   * bridge the obj's decorated names of statics to the PDB's display names (ADR-0010),
//   * recognise compiler-generated symbols (guards, dynamic initialisers, funclets).
//
// This is NOT a full undecorator. It reads the qualified name, template arguments of the common
// builtin/class/pointer kinds and the function scope of function-local statics; anything else makes
// display_name() return the decorated name unchanged (never a wrong guess).

#include <string>
#include <string_view>

namespace cml::core::bind {

// "?area@Square@@UEBAHXZ" -> "Square::area"; "??$offset@H@@YAHH@Z" -> "offset<int>";
// "?g@?1??statics_value@@YAHXZ@4UGuarded@@A" -> "statics_value::g"; "?s@?A0x1@@3HA" ->
// "(anonymous namespace)::s". Names that do not start with '?' (C symbols) are returned unchanged.
std::string display_name(std::string_view decorated);

// The unqualified identifier ("g" for the example above, "offset" for a template, "$TSS0" for a
// thread-safe-init guard). Empty for names that have none (constructors, vtables, C symbols).
std::string base_identifier(std::string_view decorated);

// The function a function-local static lives in ("statics_value"); empty otherwise.
std::string enclosing_function(std::string_view decorated);

// True when the name contains a scope that only exists inside one TU: an anonymous namespace
// ("?A0x<hash>") or a function scope ("?<digit>??"). Only such names can denote a TU-private
// entity, which is what resolution step (2) looks up.
bool is_tu_private_name(std::string_view decorated);

// The last top-level component of a PDB display name: "`anonymous namespace'::s_calls" -> "s_calls",
// "ns::Cls<int>::f" -> "f", "offset<int>" -> "offset".
std::string last_component(std::string_view pdb_display_name);

bool is_dynamic_initializer(std::string_view decorated);  // ??__E..., __cxx_global_var_init...
bool is_tu_wide_initializer(std::string_view decorated);  // _GLOBAL__sub_I_..., __cxx_global_var_init...
bool is_atexit_destructor(std::string_view decorated);    // ??__F...
bool is_funclet(std::string_view decorated);              // ?dtor$..., ?catch$..., ?filt$..., ?fin$..., ?try$...
bool is_guard_variable(std::string_view decorated);       // ?$TSS<n>@..., ?$S<n>@...

// For "??__Eg_tracker@?A0x1@@YAXXZ": the text the global's mangled name (without the leading '?')
// starts with ("g_tracker@?A0x1@@"). Empty when `decorated` is not a cl dynamic initialiser.
std::string initializer_target_prefix(std::string_view decorated);

// Names whose definitions are always patch-local fresh copies (plan 4.5): vtables, RTTI, string and
// floating-point constants, cl's compiler-generated "$..." locals.
bool is_fresh_data_name(std::string_view decorated);

}  // namespace cml::core::bind
