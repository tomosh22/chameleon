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

#include <optional>
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
bool is_guard_variable(std::string_view decorated);       // ?$TSS<n>@..., ?$S<n>@..., ??_B<scope>@5<n>
// WP-59 (R-67): clang's bitmask of an inline (externally visible) function's statics without thread-safe initialisation
// (`-fno-threadsafe-statics`, `/Zc:threadSafeInit-`): `??_B<scope>@5<n>`, ONE per function, external (a COMDAT of its own),
// written in the scope of the function's first guarded static ("??_B?1??counter@@YAHXZ@51"). Its bits go to the function's
// statics in the order they are declared. An internal-linkage function's bitmask is `?$S1@<scope>@4IA` instead.
bool is_inline_bitmask_guard(std::string_view decorated);
// Its <scope> ("??_B?1??counter@@YAHXZ@51" -> "?1??counter@@YAHXZ"), empty when `decorated` is not such a bitmask.
std::string_view inline_bitmask_scope(std::string_view decorated);

// WP-52 (R-60): the scope of a function-local static, or of a guard variable, with its name back-references resolved.
// The Microsoft mangling writes a name it has seen before in the same decorated name as one digit, '0'..'9', the index
// of that name in a table the reader fills as it goes (the static's own name first, then every simple name, class
// template and anonymous namespace of the scope, in order, at most ten). A guard and its static spell the same scope
// with other digits when one of them has a name in front of the scope that the table counts and the other does not:
// clang 18 does not count the guard's own name (`$TSS0`, `$S1`), while it counts the static's, so the scope
// `<lambda_1>::operator()` of `g::<lambda_0>::operator()` is `...QEBA?A?3@XZ` in the guard and `...QEBA?A?4@XZ` in the
// static (both meaning `<auto>`); clang 19.1 and later count `$TSS0` (never `$S1`), and cl counts it as well (Jolt's
// `?$TSS0@?1??GetRTTIOfType@JPH@@YAPEAVRTTI@2@...`, where `2` is `JPH`). `guard_name_counted` picks the spelling for a
// guard (it is ignored for any other name). Type back-references (digits in a parameter list) are kept as written: the
// parameter lists in front of them are the same in both names.
struct LocalScopeKey {
    std::string block;      // the number of the block the entity is declared in, as written ("?1?"), from the first scope
    std::string rest;       // the rest of the scope with every name written out, then '@' and the storage class ('4')
};
// Empty when the name is not function-local, or when any part of the scope cannot be read (never a guess).
std::optional<LocalScopeKey> local_scope_key(std::string_view decorated, bool guard_name_counted);

// WP-56 (R-68): a qualified name with its name back-references written out, read with a back-reference table of its own
// that starts empty: the name after ".?AV" in a type's unique name (".?AV<lambda_1>@?1???R0?1??f@@YAXXZ@QEBA@XZ@", where
// `0` is `<lambda_1>`), or the name after "??R" of a call operator's own decorated name (an operator's name is not in the
// table either). The text after the '@' that closes the qualified name is appended as written. A closure class spells the
// names of its scope with other digits than the decorated name of one of its statics does (whose table starts with the
// static's own name), so the two are compared in this form ("<lambda_1>@?1???R<lambda_1>@?1??f@@YAXXZ@QEBA@XZ@"; the
// static's side is local_scope_key's `rest`). Empty when any part cannot be read (never a guess).
std::optional<std::string> resolved_qualified_name(std::string_view qualified);
// The same for a whole decorated function name read from its start ("??R<lambda_1>@?1???R0?1??f@@YAXXZ@QEBA@XZ@QEBA@XZ":
// a call operator of the inner closure; its name and the encoding as the scope of one of its statics spells them).
std::optional<std::string> resolved_decorated_name(std::string_view decorated);

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
