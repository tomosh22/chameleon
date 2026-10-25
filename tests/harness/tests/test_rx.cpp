// The expect_stdout regex engine.
#include <string>
#include <vector>

#include "cml/ipc/control_ids.h"
#include "doctest.h"
#include "rx.h"

using namespace cmltest;

namespace {

// Returns the matched text of the leftmost match, or "<none>".
std::string find(const std::string& pattern, const std::string& text, std::size_t from = 0) {
    cml::Result<Regex> r = Regex::compile(pattern);
    INFO("pattern: " << pattern);
    REQUIRE_MESSAGE(r.has_value(), (r.has_value() ? "" : r.error().format()));
    Regex::Match m;
    if (!r->search(text, from, &m)) {
        return "<none>";
    }
    return text.substr(m.begin, m.end - m.begin);
}

void check_bad(const std::string& pattern) {
    cml::Result<Regex> r = Regex::compile(pattern);
    INFO("pattern: " << pattern);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == cml::ids::kRegexInvalid);
    CHECK(r.error().id() == "CML5420");
}

}  // namespace

TEST_CASE("rx: literals, dot and alternation") {
    CHECK(find("abc", "xxabcxx") == "abc");
    CHECK(find("abc", "ab") == "<none>");
    CHECK(find("a.c", "abc") == "abc");
    CHECK(find("a.c", "a\nc") == "<none>");
    CHECK(find("cat|dog", "hotdog") == "dog");
    CHECK(find("cat|dog|bird", "a bird") == "bird");
    CHECK(find("gr(a|e)y", "grey") == "grey");
    CHECK(find("gr(?:a|e)y", "gray") == "gray");
    CHECK(find("", "abc") == "");  // empty pattern matches the empty string
    CHECK(find("a|", "b") == "");
    CHECK(find("value=2", "TICK n=1 value=2 extras=0") == "value=2");
}

TEST_CASE("rx: classes and escapes") {
    CHECK(find("[abc]+", "xxbcaxx") == "bca");
    CHECK(find("[a-z]+", "ABCdefGHI") == "def");
    CHECK(find("[^a-z]+", "abcDEF123xyz") == "DEF123");
    CHECK(find("[0-9a-f]+", "zz1f9gz") == "1f9");
    CHECK(find("[\\d.]+", "v 1.25 x") == "1.25");
    CHECK(find("[a\\-z]+", "a-z") == "a-z");
}

TEST_CASE("rx: class escapes d w s and negations") {
    CHECK(find("\\d+", "n=1234;") == "1234");
    CHECK(find("\\D+", "12ab34") == "ab");
    CHECK(find("\\w+", "  foo_1  ") == "foo_1");
    CHECK(find("\\W+", "ab, cd") == ", ");
    CHECK(find("\\s+", "a \t b") == " \t ");
    CHECK(find("\\S+", "  xy  ") == "xy");
    CHECK(find("a\\.b", "a.b axb") == "a.b");
    CHECK(find("a\\.b", "axb") == "<none>");
    CHECK(find("\\\\", "a\\b") == "\\");
    CHECK(find("\\t", "a\tb") == "\t");
    CHECK(find("\\(x\\)", "(x)") == "(x)");
}

TEST_CASE("rx: quantifiers") {
    CHECK(find("ab*c", "ac") == "ac");
    CHECK(find("ab*c", "abbbc") == "abbbc");
    CHECK(find("ab+c", "ac") == "<none>");
    CHECK(find("ab+c", "abc") == "abc");
    CHECK(find("ab?c", "abc ac") == "abc");
    CHECK(find("a{3}", "aaaa") == "aaa");
    CHECK(find("a{2,}", "caaaab") == "aaaa");
    CHECK(find("a{2,3}", "aaaa") == "aaa");
    CHECK(find("a{2,3}", "a") == "<none>");
    CHECK(find("(ab){2}", "ababab") == "abab");
    CHECK(find("x{0}y", "xy") == "y");
    // lazy quantifiers
    CHECK(find("a.*b", "aXbXb") == "aXbXb");
    CHECK(find("a.*?b", "aXbXb") == "aXb");
    CHECK(find("a+?", "aaa") == "a");
    CHECK(find("a{1,3}?", "aaa") == "a");
    CHECK(find("a??b", "ab") == "ab");
    // a '{' that is not a quantifier is a literal
    CHECK(find("a{", "a{") == "a{");
    CHECK(find("a{x}", "a{x}") == "a{x}");
    CHECK(find("{1}", "{1}") == "{1}");
    // backtracking
    CHECK(find("(a|ab)(c|bcd)", "abcd") == "abcd");
    CHECK(find("x*x*x*y", "xxxxy") == "xxxxy");
}

TEST_CASE("rx: anchors and word boundaries are line based") {
    CHECK(find("^TICK", "TICK 1\nTICK 2") == "TICK");
    CHECK(find("^TICK", "xTICK\nTICK") == "TICK");
    Regex::Match m;
    cml::Result<Regex> r = Regex::compile("^TICK");
    REQUIRE(r.has_value());
    REQUIRE(r->search("xTICK\nTICK", 0, &m));
    CHECK(m.begin == 6);
    CHECK(find("2$", "a2\nb3") == "2");
    CHECK(find("3$", "a2\nb3") == "3");
    CHECK(find("a$", "a\r\nb") == "a");  // CRLF line ends
    CHECK(find("\\bfoo\\b", "afoo foo fooz") == "foo");
    CHECK(find("\\Boo", "foo") == "oo");
    CHECK(find("\\bfoo", "afoo") == "<none>");
    CHECK(find("^$", "a\n\nb") == "");
}

TEST_CASE("rx: search honours the start offset (cursor)") {
    const std::string text = "TICK n=0 value=1\nTICK n=1 value=1\nTICK n=2 value=2\n";
    cml::Result<Regex> r = Regex::compile("value=\\d");
    REQUIRE(r.has_value());
    Regex::Match m;
    REQUIRE(r->search(text, 0, &m));
    CHECK(m.begin == 9);
    REQUIRE(r->search(text, m.end, &m));
    CHECK(text.substr(m.begin, m.end - m.begin) == "value=1");
    CHECK(m.begin == 26);
    REQUIRE(r->search(text, m.end, &m));
    CHECK(text.substr(m.begin, m.end - m.begin) == "value=2");
    CHECK_FALSE(r->search(text, m.end, &m));
    CHECK_FALSE(r->budget_exceeded());
    CHECK_FALSE(r->search(text, text.size() + 5, &m));  // offset past the end is just "no match"
}

TEST_CASE("rx: the real scenario pattern") {
    CHECK(find("TICK n=\\d+ value=1", "NOTE logic-start\nTICK n=3 value=1 extras=7\n") == "TICK n=3 value=1");
    CHECK(find("TICK n=\\d+ value=1", "TICK n=3 value=2 extras=7\n") == "<none>");
}

TEST_CASE("rx: malformed patterns are CML5420") {
    for (const char* p : {"(", ")", "(a", "a)", "[", "[a", "[]", "[^]", "[z-a]", "*", "+a", "?", "a|*", "(*)", "\\", "[a\\",
                          "a{2,1}", "a{1001}", "a{1,1001}", "(?=a)", "(?<n>a)", "(?i)a", "\\1", "^*", "$+", "\\b+"}) {
        check_bad(p);
    }
    // nesting and program size are bounded
    std::string deep(70, '(');
    deep += "a";
    deep += std::string(70, ')');
    check_bad(deep);
    check_bad("(a{1000}){1000}");
    check_bad("((a{100}){100}){100}");
    CHECK(Regex::compile("a{1000}").has_value());
}

TEST_CASE("rx: catastrophic backtracking is cut off by the step budget instead of hanging") {
    cml::Result<Regex> r = Regex::compile("(a*)*b");
    REQUIRE(r.has_value());
    const std::string text(40, 'a');
    Regex::Match m;
    CHECK_FALSE(r->search(text, 0, &m));
    CHECK(r->budget_exceeded());
    cml::Result<Regex> nested = Regex::compile("(a+)+$");
    REQUIRE(nested.has_value());
    const std::string almost = std::string(60, 'a') + "!";
    CHECK_FALSE(nested->search(almost, 0, &m));
    CHECK(nested->budget_exceeded());
    // a normal search on a big text stays within the budget
    std::string big;
    for (int i = 0; i < 20000; ++i) big += "TICK n=" + std::to_string(i) + " value=1\n";
    cml::Result<Regex> normal = Regex::compile("value=2");
    REQUIRE(normal.has_value());
    CHECK_FALSE(normal->search(big, 0, &m));
    CHECK_FALSE(normal->budget_exceeded());
}

TEST_CASE("rx: unbounded repeats of a body that can match the empty string terminate (review fix 1)") {
    CHECK(find("(?:a*)*b", "b") == "b");
    CHECK(find("(?:a*)*b", "xaab") == "aab");
    CHECK(find("(a|)*b", "aab") == "aab");
    CHECK(find("(?:)*x", "x") == "x");
    CHECK(find("(?:a?)+$", "aa") == "aa");
    CHECK(find("(?:a?)+?$", "aa") == "aa");
    CHECK(find("(a*)+c", "aac") == "aac");
    CHECK(find("((a|b?)*)*z", "abz") == "abz");
    // none of these may need the step budget
    for (const char* pattern : {"(?:a*)*b", "(a|)*b", "(?:)*x", "(?:a?)+$"}) {
        cml::Result<Regex> r = Regex::compile(pattern);
        REQUIRE(r.has_value());
        Regex::Match m;
        (void)r->search("aab", 0, &m);
        CHECK_FALSE(r->budget_exceeded());
    }
    // a loop whose body matched empty after real iterations leaves the loop and goes on
    CHECK(find("(?:a|b*)*c", "abbac") == "abbac");
}
