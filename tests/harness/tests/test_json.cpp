// JSON reader/writer of the control protocol: valid documents, round trips, and a long list of
// malformed inputs that must all be rejected with CML5400 (never crash, never accept).
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "cml/ipc/control_ids.h"
#include "cml/ipc/json.h"
#include "doctest.h"

using namespace cml;
using namespace cml::ipc::json;

namespace {

Value parse_ok(const std::string& text) {
    Result<Value> r = parse(text);
    REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? "" : r.error().format()));
    return *r;
}

void check_rejected(const std::string& text) {
    Result<Value> r = parse(text);
    INFO("input: " << text.substr(0, 80));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == ids::kControlJsonMalformed);
    CHECK(r.error().id() == "CML5400");
    CHECK_FALSE(r.error().what_to_do.empty());
}

}  // namespace

TEST_CASE("json: scalars") {
    CHECK(parse_ok("null").is_null());
    CHECK(parse_ok("true").as_bool() == true);
    CHECK(parse_ok("false").is_bool());
    CHECK(parse_ok("0").as_integer(7) == 0);
    CHECK(parse_ok("-17").as_integer() == -17);
    CHECK(parse_ok("9223372036854775807").as_integer() == INT64_MAX);
    CHECK(parse_ok("-9223372036854775808").as_integer() == INT64_MIN);
    CHECK(parse_ok("1.5").as_double() == doctest::Approx(1.5));
    CHECK(parse_ok("1.5").is_number());
    CHECK_FALSE(parse_ok("1.5").is_integer());
    CHECK(parse_ok("2e3").as_double() == doctest::Approx(2000.0));
    CHECK(parse_ok("-1.25E-2").as_double() == doctest::Approx(-0.0125));
    // an integer too large for int64 becomes a double
    const Value big = parse_ok("9223372036854775808");
    CHECK_FALSE(big.is_integer());
    CHECK(big.as_double() > 9.2e18);
    CHECK(parse_ok("  \t\r\n 42 \n").as_integer() == 42);
}

TEST_CASE("json: strings and escapes") {
    CHECK(parse_ok("\"\"").as_string().empty());
    CHECK(parse_ok("\"abc\"").as_string() == "abc");
    CHECK(parse_ok("\"a\\\"b\\\\c\\/d\"").as_string() == "a\"b\\c/d");
    CHECK(parse_ok("\"\\b\\f\\n\\r\\t\"").as_string() == "\b\f\n\r\t");
    CHECK(parse_ok("\"\\u0041\\u00e9\"").as_string() == "A\xC3\xA9");
    CHECK(parse_ok("\"\\u20AC\"").as_string() == "\xE2\x82\xAC");
    // surrogate pair -> U+1F600
    CHECK(parse_ok("\"\\uD83D\\uDE00\"").as_string() == "\xF0\x9F\x98\x80");
    // raw UTF-8 passes through
    CHECK(parse_ok("\"caf\xC3\xA9\"").as_string() == "caf\xC3\xA9");
}

TEST_CASE("json: arrays and objects") {
    const Value a = parse_ok("[1, \"x\", [true], {}]");
    REQUIRE(a.is_array());
    REQUIRE(a.items().size() == 4);
    CHECK(a.items()[0].as_integer() == 1);
    CHECK(a.items()[1].as_string() == "x");
    CHECK(a.items()[2].items()[0].as_bool());
    CHECK(a.items()[3].is_object());
    CHECK(parse_ok("[]").items().empty());

    const Value o = parse_ok("{\"id\": 3, \"cmd\": \"status\", \"args\": {\"k\": [1,2]}}");
    REQUIRE(o.is_object());
    CHECK(o.get_integer("id") == 3);
    CHECK(o.get_string("cmd") == "status");
    REQUIRE(o.find("args") != nullptr);
    CHECK(o.find("args")->find("k")->items().size() == 2);
    CHECK(o.find("missing") == nullptr);
    CHECK(o.get_integer("cmd", -1) == -1);  // wrong type -> fallback
    CHECK(o.get_string("id", "fb") == "fb");
    CHECK(o.get_bool("nope", true));
    CHECK(parse_ok("{}").size() == 0);
}

TEST_CASE("json: serialisation is compact, single line and round-trips") {
    Value o = Value::object();
    o.set("s", Value::string("line1\nline2\t\"q\" \\ \x01"));
    o.set("n", Value::integer(-5));
    o.set("d", Value::number(2.5));
    o.set("one", Value::number(1.0));
    o.set("b", Value::boolean(true));
    o.set("z", Value());
    Value arr = Value::array();
    arr.push(Value::integer(1));
    arr.push(Value::string("\xC3\xA9"));
    o.set("a", arr);
    const std::string text = serialize(o);
    CHECK(text.find('\n') == std::string::npos);
    CHECK(text.find('\x01') == std::string::npos);
    CHECK(text.find("\\u0001") != std::string::npos);
    const Value back = parse_ok(text);
    CHECK(back.get_string("s") == o.get_string("s"));
    CHECK(back.get_integer("n") == -5);
    CHECK(back.find("d")->as_double() == doctest::Approx(2.5));
    CHECK(back.find("one")->type() == Type::Double);  // "1.0", not "1"
    CHECK(back.get_bool("b"));
    CHECK(back.find("z")->is_null());
    CHECK(back.find("a")->items()[1].as_string() == "\xC3\xA9");
    CHECK(serialize(back) == text);
    CHECK(serialize(Value()) == "null");
}

TEST_CASE("json: set() replaces, non-finite doubles become null, invalid UTF-8 is repaired on output") {
    Value o = Value::object();
    o.set("k", Value::integer(1));
    o.set("k", Value::integer(2));
    CHECK(o.size() == 1);
    CHECK(o.get_integer("k") == 2);
    Value d = Value::number(std::numeric_limits<double>::quiet_NaN());
    CHECK(serialize(d) == "null");
    Value bad = Value::string(std::string("a\xFF" "b"));
    const std::string out = serialize(bad);
    CHECK(parse(out).has_value());  // the writer never emits invalid UTF-8
}

TEST_CASE("json: malformed documents are rejected with CML5400") {
    const std::vector<std::string> bad = {
        "",
        "   ",
        "nul",
        "tru",
        "True",
        "[",
        "]",
        "{",
        "}",
        "[1,]",
        "[,1]",
        "[1 2]",
        "{\"a\":1,}",
        "{\"a\" 1}",
        "{\"a\":}",
        "{a:1}",
        "{1:2}",
        "{\"a\":1 \"b\":2}",
        "'single'",
        "\"unterminated",
        "\"bad\\escape\"",
        "\"\\u12\"",
        "\"\\u12G4\"",
        "\"\\uD800\"",
        "\"\\uD800x\"",
        "\"\\uD800\\u0041\"",
        "\"\\uDC00\"",
        "\"raw\nnewline\"",
        "\"raw\ttab\"",
        "01",
        "-",
        "-a",
        "1.",
        ".5",
        "1e",
        "1e+",
        "+1",
        "0x10",
        "NaN",
        "Infinity",
        "1e999",
        "[1] [2]",
        "{} x",
        "{\"a\":1,\"a\":2}",
        "\"\xFF\"",
        "\"\xC0\xAF\"",
        "\"\xED\xA0\x80\"",
        "/* c */ 1",
    };
    for (const std::string& text : bad) {
        check_rejected(text);
    }
}

TEST_CASE("json: truncating a valid document anywhere never crashes and never succeeds early") {
    const std::string doc = "{\"id\":12,\"cmd\":\"status\",\"args\":{\"list\":[1,2.5,\"s\\u00e9\",null,true],\"o\":{}}}";
    REQUIRE(parse(doc).has_value());
    for (std::size_t n = 0; n < doc.size(); ++n) {
        check_rejected(doc.substr(0, n));
    }
}

TEST_CASE("json: limits") {
    // depth: 32 nested arrays are fine, 33 are not
    std::string ok_depth(32, '[');
    ok_depth += std::string(32, ']');
    CHECK(parse(ok_depth).has_value());
    std::string deep(33, '[');
    deep += std::string(33, ']');
    check_rejected(deep);
    std::string very_deep(100000, '[');
    check_rejected(very_deep);  // no stack overflow
    std::string deep_obj;
    for (int i = 0; i < 40; ++i) deep_obj += "{\"a\":";
    deep_obj += "1";
    deep_obj += std::string(40, '}');
    check_rejected(deep_obj);

    // string length
    check_rejected("\"" + std::string(kMaxStringBytes + 1, 'x') + "\"");
    CHECK(parse("\"" + std::string(kMaxStringBytes, 'x') + "\"").has_value());

    // document size
    check_rejected("\"" + std::string(kMaxDocumentBytes, 'x') + "\"");
    check_rejected(std::string(kMaxDocumentBytes + 1, ' ') + "1");

    // element count
    std::string many = "[";
    for (std::size_t i = 0; i < kMaxElements + 1; ++i) {
        many += i == 0 ? "0" : ",0";
    }
    many += "]";
    check_rejected(many);
    std::string fits = "[";
    for (std::size_t i = 0; i < kMaxElements; ++i) {
        fits += i == 0 ? "0" : ",0";
    }
    fits += "]";
    CHECK(parse(fits).has_value());
}

TEST_CASE("json: an error names the byte offset") {
    Result<Value> r = parse("{\"a\": ?}");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("byte 6") != std::string::npos);
}

TEST_CASE("json: objects are capped at 4096 members and duplicate detection is linear (review fix 2)") {
    auto object_with = [](std::size_t n) {
        std::string text = "{";
        for (std::size_t i = 0; i < n; ++i) {
            text += (i == 0 ? "\"k" : ",\"k") + std::to_string(i) + "\":" + std::to_string(i);
        }
        return text + "}";
    };
    CHECK(parse(object_with(kMaxMembers)).has_value());
    check_rejected(object_with(kMaxMembers + 1));
    // a duplicate of the first key at the very end of a full object is still found
    std::string full = object_with(kMaxMembers - 1);
    full.pop_back();
    full += ",\"k0\":1}";
    check_rejected(full);
    // 65k keys: rejected fast (the cap), never quadratic
    const auto start = std::chrono::steady_clock::now();
    check_rejected(object_with(65000));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    CHECK(ms < 100);
    // many equal keys: the first duplicate is reported at once
    std::string dups = "{";
    for (int i = 0; i < 3000; ++i) dups += i == 0 ? "\"a\":1" : ",\"a\":1";
    dups += "}";
    check_rejected(dups);
}
