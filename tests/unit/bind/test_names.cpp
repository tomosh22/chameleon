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
    CHECK(base_identifier("??0Square@@QEAA@H@Z").empty());
    CHECK(base_identifier("??_7Square@@6B@").empty());
    CHECK(base_identifier("fixture_note").empty());
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
