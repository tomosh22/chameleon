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

// WP-37: the scope of a function-local static whose enclosing function is an operator() (a lambda's call operator).
// "?sname@?1???R<lambda_1>@?1??f@@YAXXZ@QEBAAEBUName@@XZ@4U2@A" -> "<lambda_1>@?1??f@@YAXXZ@QEBAAEBUName@@XZ@4U2@A":
// the text that follows "??R" of the nested function name. It BEGINS with the qualified name of the operator's class,
// spelled exactly as that class's unique name in the type records spells it after ".?AV" (".?AV<lambda_1>@?1??f@@YAXXZ@"),
// so a closure is recognised by a prefix comparison; nothing of the name is interpreted. Empty for any other name.
std::string_view call_operator_scope(std::string_view decorated);

// WP-26c: the identity of a C static whose object name is not Microsoft-decorated, as clang-cl spells it for a
// translation unit compiled as C. The PDB records carry the source names ("platform", "counter"); the object's are
//   file scope "platform"; a function-local static "next.counter"; LLVM's uniquifier appends ".<n>" when the name is
//   taken ("platform.0", "next.counter.1").
// (cl decorates C statics like C++ ones, "?counter@?1??next@@9@9", observed in CI; that is display_name()'s job.)
// `base` is empty when no source name can be read ("__const.f.x", ".str", "$SG1").
struct CStaticName {
    std::string base;
    std::string encl;
};
CStaticName c_static_name(std::string_view object_name);

// WP-26c: names of compiler-generated data the user never wrote, as an explicit list. The planner treats such data as new
// (a fresh copy is right for it) and never refuses a reload because it cannot read a source name from it:
//   clang: ".compoundliteral[.N]", ".str[.N]", "__const.*", "__func__.*", "__profc_*" / "__profd_*" / "__profn_*" (PGO),
//          "__covrec_*" / "__llvm_*" (coverage), "___asan_*" / "__asan_*" / "__odr_*" / "__sancov*" (sanitizers)
//   cl/clang decorated names that start with '?' (guards, vtables, RTTI, string literals: they are never user spellings)
//   "$..." (cl's "$SG", "$LN") and "__real@" / "__xmm@" constants.
bool is_compiler_generated_data_name(std::string_view name);

// WP-40 (R-48 item 4): a name with a trailing ".<digits>" behind a name of a variable: a decorated name
// ("?s_last@?A0x520C9D4F@@3PAHA.0") or a C identifier ("s_last.1", "next.counter.1"). clang-cl names the pieces of an
// aggregate it split (SROA of a global) like that, and LLVM's uniquifier writes the same suffix; the planner tells the
// two apart by the debug records of the object, not by the name.
bool is_split_piece_name(std::string_view name);

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

// True when the cl dynamic initialiser `initializer` (??__E...) belongs to the global `global`:
//   ??__Eg@?A0x1@@YAXXZ      <-> ?g@?A0x1@@3UT@@A     (ordinary globals)
//   ??__E?x@Foo@@2HA@@YAXXZ  <-> ?x@Foo@@2HA          (static data members)
//   ??__E?$v@H@@YAXXZ        <-> ??$v@H@@3HA          (variable templates)
bool initializer_matches_global(std::string_view initializer, std::string_view global);

// Names whose definitions are always patch-local fresh copies (plan 4.5): vtables, RTTI, string and
// floating-point constants, cl's compiler-generated "$..." locals.
bool is_fresh_data_name(std::string_view decorated);

}  // namespace cml::core::bind
