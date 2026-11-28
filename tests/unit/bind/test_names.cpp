// WP-07: name helpers (display names, base identifiers, generated-symbol recognisers).
#include "cml/core/bind_names.h"

#include "doctest.h"

using namespace cml::core::bind;

TEST_CASE("display_name: plain, qualified and C names") {
    CHECK(display_name("?compute_value@@YAHXZ") == "compute_value");
    CHECK(display_name("?area@Square@@UEBAHXZ") == "Square::area");
    CHECK(display_name("?total@Accum@@2HA") == "Accum::total");
    CHECK(display_name("?f@inner@outer@@YAXXZ") == "outer::inner::f");
    CHECK(display_name("fixture_note") == "fixture_note");
    CHECK(display_name("__security_check_cookie") == "__security_check_cookie");
    CHECK(display_name("") == "");
}

TEST_CASE("display_name: anonymous namespaces, function-local statics, guards") {
    CHECK(display_name("?s_calls@?A0xdeadbeef@@3HA") == "(anonymous namespace)::s_calls");
    CHECK(display_name("?g@?1??statics_value@@YAHXZ@4UGuarded@@A") == "statics_value::g");
    CHECK(display_name("?$TSS0@?1??statics_value@@YAHXZ@4HA") == "statics_value::$TSS0");
    CHECK(display_name("?g_tracker@?A0x1234abcd@@3UTracker@@A") == "(anonymous namespace)::g_tracker");
}

TEST_CASE("display_name: templates") {
    CHECK(display_name("??$offset@H@@YAHH@Z") == "offset<int>");
    CHECK(display_name("??$offset@N@@YANN@Z") == "offset<double>");
    CHECK(display_name("??$twice@H@@YAHH@Z") == "twice<int>");
    CHECK(display_name("?size@?$vector@H@std@@QEBA_KXZ") == "std::vector<int>::size");
    CHECK(display_name("?f@?$Box@PEAH@@QEAAXXZ") == "Box<int*>::f");
    CHECK(display_name("?f@?$Box@$0A@@@QEAAXXZ") == "Box<0>::f");
    CHECK(display_name("?f@?$Box@$00@@QEAAXXZ") == "Box<1>::f");
    CHECK(display_name("?f@?$Box@$0BA@@@QEAAXXZ") == "Box<16>::f");
    CHECK(display_name("?f@?$Box@V?$Inner@H@ns@@@@QEAAXXZ") == "Box<ns::Inner<int>>::f");
}

TEST_CASE("display_name: special members and generated symbols") {
    CHECK(display_name("??0Square@@QEAA@H@Z") == "Square::Square");
    CHECK(display_name("??1Square@@UEAA@XZ") == "Square::~Square");
    CHECK(display_name("??_7Square@@6B@") == "Square::`vftable'");
    CHECK(display_name("??_GSquare@@UEAAPEAXI@Z") == "Square::`scalar deleting destructor'");
    CHECK(display_name("??__Eg_tracker@?A0x1@@YAXXZ") == "`dynamic initializer for '(anonymous namespace)::g_tracker''");
    CHECK(display_name("??__Fg_tracker@?A0x1@@YAXXZ") == "`dynamic atexit destructor for '(anonymous namespace)::g_tracker''");
    CHECK(display_name("??RFoo@@QEBAHXZ") == "Foo::operator()");
    CHECK(display_name("??HFoo@@QEBAHH@Z") == "Foo::operator+");
    CHECK(display_name("??2@YAPEAX_K@Z") == "operator new");
    CHECK(display_name("??3@YAXPEAX_K@Z") == "operator delete");
    CHECK(display_name("??R<lambda_1>@?0??lambdas_init@@YAXH@Z@QEBA?A?<auto>@@H@Z") == "lambdas_init::<lambda_1>::operator()");
}

TEST_CASE("display_name: undecodable input is returned unchanged, never guessed") {
    CHECK(display_name("?f@?$Box@$$QEAH@@QEAAXXZ") == "?f@?$Box@$$QEAH@@QEAAXXZ");
    CHECK(display_name("?truncated") == "?truncated");
    CHECK(display_name("?") == "?");
}

TEST_CASE("base_identifier and enclosing_function") {
    CHECK(base_identifier("?compute_value@@YAHXZ") == "compute_value");
    CHECK(base_identifier("?g@?1??statics_value@@YAHXZ@4UGuarded@@A") == "g");
    CHECK(base_identifier("??$offset@H@@YAHH@Z") == "offset");
    CHECK(base_identifier("?$TSS0@?1??statics_value@@YAHXZ@4HA") == "$TSS0");
    CHECK(base_identifier("??0Square@@QEAA@H@Z") == "Square");
    CHECK(base_identifier("??1Square@@UEAA@XZ") == "~Square");
    CHECK(base_identifier("??0?$Box@H@ns@@QEAA@XZ") == "Box");
    CHECK(base_identifier("??R<lambda_0>@?0??lambdas_init@@YAXH@Z@QEBA?A?<auto>@@H@Z") == "operator()");
    CHECK(base_identifier("??HFoo@@QEBAHH@Z") == "operator+");
    CHECK(base_identifier("??_7Square@@6B@").empty());
    CHECK(base_identifier("k_weights") == "k_weights");  // clang: internal data keeps its plain name
    CHECK(base_identifier("$SG1234").empty());
    CHECK(base_identifier("__real@3ff0000000000000").empty());
    CHECK(enclosing_function("?g@?1??statics_value@@YAHXZ@4UGuarded@@A") == "statics_value");
    CHECK(enclosing_function("?s_calls@?A0x1@@3HA").empty());
    CHECK(enclosing_function("?compute_value@@YAHXZ").empty());
}

TEST_CASE("is_tu_private_name") {
    CHECK(is_tu_private_name("?s_calls@?A0xdeadbeef@@3HA"));
    CHECK(is_tu_private_name("?g@?1??statics_value@@YAHXZ@4UGuarded@@A"));
    CHECK_FALSE(is_tu_private_name("?g_tick@@3HA"));
    CHECK_FALSE(is_tu_private_name("fixture_note"));
}

TEST_CASE("last_component of PDB display names") {
    CHECK(last_component("compute_value") == "compute_value");
    CHECK(last_component("`anonymous namespace'::s_calls") == "s_calls");
    CHECK(last_component("(anonymous namespace)::s_calls") == "s_calls");
    CHECK(last_component("ns::Cls<int>::f") == "f");
    CHECK(last_component("offset<int>") == "offset");
    CHECK(last_component("std::map<int, std::pair<int, int>>::size") == "size");
    CHECK(last_component("A::B::C") == "C");
}

TEST_CASE("generated-symbol recognisers") {
    CHECK(is_dynamic_initializer("??__Eg_tracker@?A0x1@@YAXXZ"));
    CHECK(is_dynamic_initializer("__cxx_global_var_init.1"));
    CHECK_FALSE(is_dynamic_initializer("?compute_value@@YAHXZ"));
    CHECK(is_tu_wide_initializer("_GLOBAL__sub_I_logic.cpp"));
    CHECK(is_atexit_destructor("??__Fg_tracker@?A0x1@@YAXXZ"));
    CHECK(is_funclet("?dtor$2@?0??f@@YAHXZ@4HA"));
    CHECK(is_funclet("?catch$3@?0??f@@YAHXZ@4HA"));
    CHECK_FALSE(is_funclet("?dtor@Foo@@QEAAXXZ"));
    CHECK(is_guard_variable("?$TSS0@?1??statics_value@@YAHXZ@4HA"));
    CHECK(is_guard_variable("?$S1@?1??f@@YAHXZ@4IA"));
    CHECK_FALSE(is_guard_variable("?$TSSx@?1??f@@YAHXZ@4HA"));
    CHECK_FALSE(is_guard_variable("?g@?1??f@@YAHXZ@4HA"));
    CHECK(is_fresh_data_name("??_7Square@@6B@"));
    CHECK(is_fresh_data_name("??_R4Square@@6B@"));
    CHECK(is_fresh_data_name("??_C@_05ABC@hello?$AA@"));
    CHECK(is_fresh_data_name("__real@3ff0000000000000"));
    CHECK(is_fresh_data_name("$SG1234"));
    CHECK_FALSE(is_fresh_data_name("?g_tick@@3HA"));
}

TEST_CASE("initializer_target_prefix") {
    CHECK(initializer_target_prefix("??__Eg_tracker@?A0x1@@YAXXZ") == "g_tracker@?A0x1@@");
    CHECK(initializer_target_prefix("??__E?x@Foo@@2HA@@YAXXZ") == "?x@Foo@@2HA");
    CHECK(initializer_target_prefix("?compute_value@@YAHXZ").empty());
}

// WP-37: cl writes the block a local entity was declared in as one digit up to block 10 and in letters from block 11 on
// ("?L@?": Godot's SceneTree::_flush_scene_change, corpus run 45438434773). The reader knew only the digit.
TEST_CASE("display_name: function scopes with a block number written in letters") {
    CHECK(display_name("?sname@?1???R<lambda_1>@?L@??_flush_scene_change@SceneTree@@AEAAXXZ@QEBAAEBVStringName@@XZ@4V4@A") ==
          "SceneTree::_flush_scene_change::<lambda_1>::operator()::sname");
    CHECK(display_name("?$TSS0@?1???R<lambda_1>@?N@??process_timers@SceneTree@@AEAAXN_N@Z@QEBAAEBVStringName@@XZ@4HA") ==
          "SceneTree::process_timers::<lambda_1>::operator()::$TSS0");
    CHECK(display_name("??R<lambda_1>@?L@??_flush_scene_change@SceneTree@@AEAAXXZ@QEBAAEBVStringName@@XZ") == "SceneTree::_flush_scene_change::<lambda_1>::operator()");
    CHECK(display_name("?x@?L@??f@@YAXXZ@4HA") == "f::x");
    CHECK(display_name("?x@?BA@??f@@YAXXZ@4HA") == "f::x");  // block 17
    CHECK(display_name("?x@?9??f@@YAXXZ@4HA") == "f::x");    // block 10: still a digit
    CHECK(enclosing_function("?x@?L@??f@@YAXXZ@4HA") == "f");
    CHECK(enclosing_function("?names@?4??get_argument_options@SceneTree@@UEBAXAEBVStringName@@H@Z@4HB") == "get_argument_options");
    CHECK(base_identifier("?x@?L@??f@@YAXXZ@4HA") == "x");
    CHECK(is_tu_private_name("?x@?L@??f@@YAXXZ@4HA"));
    CHECK(is_tu_private_name("?x@?BA@??f@@YAXXZ@4HA"));
    // Not a function scope: an anonymous namespace, a template, letters without the nested name.
    CHECK(display_name("?g@?A0xABCDEFAB@@3HA") == "(anonymous namespace)::g");
    CHECK(display_name("?f@?$Box@H@@QEAAXXZ") == "Box<int>::f");
    CHECK_FALSE(is_tu_private_name("?f@?$Box@H@@QEAAXXZ"));
    CHECK_FALSE(is_tu_private_name("?g_tick@AB@@3HA"));
}

// ---- WP-52 (R-60): scopes with their name back-references resolved -----------------------------------------------------

namespace {

std::string rest_of(std::string_view name, bool guard_name_counted = false) {
    const std::optional<LocalScopeKey> k = local_scope_key(name, guard_name_counted);
    return k ? k->rest : std::string("(none)");
}

}  // namespace

TEST_CASE("local_scope_key: clang 18 spells a guard's scope with back-references one lower than its static's") {
    // clang 18 -O2 (x86_64-pc-windows-msvc), `void g() { auto o = [] { auto i = [] { static Obj gx = make(1); ... }; ... }; }`:
    // the guard does not count its own name `$TSS0`, the static counts `gx`, so `<auto>` is 3 in one and 4 in the other.
    const char* guard = "?$TSS0@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?3@XZ@4HA";
    const char* gx = "?gx@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?4@XZ@4UObj@@A";
    const std::optional<LocalScopeKey> g = local_scope_key(guard, false);
    const std::optional<LocalScopeKey> s = local_scope_key(gx, false);
    REQUIRE(g.has_value());
    REQUIRE(s.has_value());
    CHECK(g->block == "?1?");
    CHECK(g->rest == s->rest);
    CHECK(s->rest == "??R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?<auto>@@XZ@4");
    // In cl's spelling (the guard's own name counted) the guard's `3` is `g`: another scope.
    CHECK(rest_of(guard, true) != s->rest);
    // The bitmask of -fno-threadsafe-statics, the same scope.
    CHECK(rest_of("?$S1@?1???R<lambda_1>@?0???R<lambda_0>@?0??g@@YAXXZ@QEBA?A?<auto>@@XZ@QEBA?A?3@XZ@4IA") == s->rest);
    // A split piece of the static (clang /O2 SROA) has the same scope.
    CHECK(rest_of(std::string(gx) + ".0") == s->rest);
}

TEST_CASE("local_scope_key: templates, parameter types, anonymous namespaces (clang 18)") {
    // A class template is one name of the table, a function template's own name is none; type back-references are kept.
    CHECK(rest_of("?$TSS0@?1???$tf@H@@YAHU?$Box@H@@UFoo@@PEAU1@2@Z@4HA") == rest_of("?tx@?1???$tf@H@@YAHU?$Box@H@@UFoo@@PEAU2@2@Z@4UObj@@A"));
    CHECK(rest_of("?tx@?1???$tf@H@@YAHU?$Box@H@@UFoo@@PEAU2@2@Z@4UObj@@A") == "??$tf@H@@YAHU?$Box@H@@UFoo@@PEAUFoo@@2@Z@4");
    CHECK(rest_of("?$TSS0@?1??GetRTTIOfType@JPH@@YAPEAURTTI@1@PEAUSphereShape@1@@Z@4HA") ==
          rest_of("?rtti@?1??GetRTTIOfType@JPH@@YAPEAURTTI@2@PEAUSphereShape@2@@Z@4U32@A"));
    CHECK(rest_of("?$TSS0@?1???R<lambda_1>@?0??anon@?A0x47809CB@@YAHUFoo@@@Z@QEBA?A?<auto>@@0@Z@4HA") ==
          rest_of("?lx@?1???R<lambda_1>@?0??anon@?A0x47809CB@@YAHUFoo@@@Z@QEBA?A?<auto>@@0@Z@4UObj@@A.0"));
    CHECK(rest_of("?$TSS0@?1??m@K@@QEBAHH@Z@4HA") == rest_of("?kx@?1??m@K@@QEBAHH@Z@4UObj@@A"));
}

TEST_CASE("resolved_qualified_name (WP-56, R-68): a closure's unique name and its static's scope, resolved, begin alike") {
    // cl (planner_pairing_o2/nested.cpp, CI): the static's table starts with `nested`, so the outer closure `<lambda_1>` is
    // its `1`; the inner closure's unique name has a table of its own, where it is `0`.
    const char* nested = "?nested@?1???R<lambda_1>@?1???R1?1??o2_nest_step@@YAXXZ@QEBA@XZ@QEBA@XZ@4UO2Name@@A";
    const std::optional<std::string> closure = resolved_qualified_name("<lambda_1>@?1???R0?1??o2_nest_step@@YAXXZ@QEBA@XZ@");
    REQUIRE(closure.has_value());
    CHECK(*closure == "<lambda_1>@?1???R<lambda_1>@?1??o2_nest_step@@YAXXZ@QEBA@XZ@");
    const std::string scope = rest_of(nested);
    REQUIRE(scope.size() > 3);
    CHECK(scope.compare(0, 3, "??R") == 0);
    CHECK(scope.compare(3, closure->size(), *closure) == 0);
    // As written they differ (the comparison before WP-56).
    CHECK(std::string(call_operator_scope(nested)).compare(0, 21, "<lambda_1>@?1???R0?1?") != 0);
    // The outer closure, and what follows the closing '@' is kept as written (cl's discriminator).
    CHECK((resolved_qualified_name("<lambda_1>@?1??o2_nest_step@@YAXXZ@") == std::optional<std::string>("<lambda_1>@?1??o2_nest_step@@YAXXZ@")));
    CHECK((resolved_qualified_name("<lambda_1>@?1??f@?A0x790cee12@@YAXXZ@`790cee12") == std::optional<std::string>("<lambda_1>@?1??f@?A0x790cee12@@YAXXZ@`790cee12")));
    // A back-reference outside the table, or nothing to read: no name.
    CHECK_FALSE(resolved_qualified_name("<lambda_1>@?1???R5?1??f@@YAXXZ@QEBA@XZ@").has_value());
    CHECK_FALSE(resolved_qualified_name("").has_value());
    CHECK_FALSE(resolved_qualified_name("@").has_value());
    // A call operator's own decorated name: its table starts with the closure (an operator's name is not memorised).
    const std::optional<std::string> op = resolved_decorated_name("??R<lambda_1>@?1???R0?1??o2_nest_step@@YAXXZ@QEBA@XZ@QEBA@XZ");
    REQUIRE(op.has_value());
    CHECK(*op == "??R<lambda_1>@?1???R<lambda_1>@?1??o2_nest_step@@YAXXZ@QEBA@XZ@QEBA@XZ");
    CHECK(scope.compare(0, op->size(), *op) == 0);
    CHECK_FALSE(resolved_decorated_name("??R<lambda_1>@?1???R7?1??f@@YAXXZ@QEBA@XZ@QEBA@XZ").has_value());
    // (Review of 6dc968a, finding 3.) The names of the types are written out too: a lambda of `JPH::Foo::bar()` returning
    // `JPH::Baz *`, whose call operator spells JPH `3` (`43` in its return type) where its static spells it `4` (`54`).
    const std::optional<std::string> jph = resolved_decorated_name("??R<lambda_1>@?1??bar@Foo@JPH@@QEAAPEAVBaz@3@XZ@QEBAPEAV43@XZ");
    REQUIRE(jph.has_value());
    CHECK(*jph == "??R<lambda_1>@?1??bar@Foo@JPH@@QEAAPEAVBaz@JPH@@XZ@QEBAPEAVBaz@JPH@@XZ");
    CHECK(rest_of("?x@?1???R<lambda_1>@?1??bar@Foo@JPH@@QEAAPEAVBaz@4@XZ@QEBAPEAV54@XZ@4HA") == *jph + "@4");
    CHECK_FALSE(resolved_decorated_name("f").has_value());
}

TEST_CASE("local_scope_key: cl counts the guard's own name (Jolt's GetRTTIOfType, Godot's SceneTree)") {
    const char* guard = "?$TSS0@?1??GetRTTIOfType@JPH@@YAPEAVRTTI@2@PEAVSphereShape@2@@Z@4HA";
    const char* rtti = "?rtti@?1??GetRTTIOfType@JPH@@YAPEAVRTTI@2@PEAVSphereShape@2@@Z@4V32@A";
    CHECK(rest_of(guard, true) == rest_of(rtti));
    CHECK(rest_of(rtti) == "?GetRTTIOfType@JPH@@YAPEAVRTTI@JPH@@PEAVSphereShape@JPH@@@Z@4");
    CHECK(rest_of(guard, false) != rest_of(rtti));  // clang's spelling: `2` would be `RTTI`
    // Block 11 and later in letters; a class back-reference in the operator's return type.
    const std::optional<LocalScopeKey> k = local_scope_key("?sname@?1???R<lambda_1>@?L@??_flush_scene_change@SceneTree@@AEAAXXZ@QEBAAEBVStringName@@XZ@4V4@A", false);
    REQUIRE(k.has_value());
    CHECK(k->rest == "??R<lambda_1>@?L@??_flush_scene_change@SceneTree@@AEAAXXZ@QEBAAEBVStringName@@XZ@4");
    CHECK(rest_of("?$TSS0@?1???R<lambda_1>@?L@??_flush_scene_change@SceneTree@@AEAAXXZ@QEBAAEBVStringName@@XZ@4HA", true) == k->rest);
    CHECK(rest_of("?$TSS0@?1???R<lambda_1>@?L@??_flush_scene_change@SceneTree@@AEAAXXZ@QEBAAEBVStringName@@XZ@4HA", false) == k->rest);
}

TEST_CASE("local_scope_key: no key for names that are not function-local or cannot be read") {
    CHECK_FALSE(local_scope_key("?g@?A0xABCDEFAB@@3HA", false).has_value());
    CHECK_FALSE(local_scope_key("?f@?$Box@H@@QEAAXXZ", false).has_value());
    CHECK_FALSE(local_scope_key("?s_a@?A0x1@@3HA", false).has_value());
    CHECK_FALSE(local_scope_key("fixture_note", false).has_value());
    CHECK_FALSE(local_scope_key("?x@?1??f@@YAXXZ", false).has_value());          // truncated: no storage class
    CHECK_FALSE(local_scope_key("?x@?1??f@@YAXL@Z@4HA", false).has_value());     // a type that is no type
    CHECK_FALSE(local_scope_key("?x@?1??f@@YAXU7@@Z@4HA", false).has_value());   // a name back-reference that is not in the table
    CHECK_FALSE(local_scope_key("?x@?1??f@@$$J0YAXXZ@4HA", false).has_value());  // extern "C" encoding: not read
    CHECK(local_scope_key("?x@?1??f@@YAXXZ@4HA", false).has_value());
    // A member pointer parameter is not read (the fallback compares the function as written).
    CHECK_FALSE(local_scope_key("?q@?1??pm@@YAHP8K@@EAAXXZ@Z@4URegistry@@A", false).has_value());
    // cl's statics of C functions (the agent's own, as the corpus logs show them).
    CHECK(rest_of("?cml_module_anchor@?1??cml_get_current_module_path@@9@4HA") == "?cml_get_current_module_path@@9@4");
    CHECK(rest_of("?$S1@?1??cml_get_current_module_path@@9@4IA", true) == "?cml_get_current_module_path@@9@4");
}

TEST_CASE("local_scope_key (review): a lambda of another function in a template argument and a parameter is read") {
    // clang 18: `template <class F> int run(F f) { static Registry r; ... }` with a lambda of main (`?main@@9`).
    const char* r = "?r@?1???$run@V<lambda_0>@?0??main@@9@@@YAHV<lambda_0>@?0??main@@9@@Z@4URegistry@@A";
    REQUIRE(local_scope_key(r, false).has_value());
    CHECK(rest_of("?$TSS0@?1???$run@V<lambda_0>@?0??main@@9@@@YAHV<lambda_0>@?0??main@@9@@Z@4HA") == rest_of(r));
}
