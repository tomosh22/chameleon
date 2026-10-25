// Tokenisation, quoting and response files (WP-05).
#include "cml/toolchain/command_line.h"
#include "cml/toolchain/diag_ids.h"

#include <map>
#include <string>
#include <vector>

#include "doctest.h"

using namespace cml::toolchain;
using Tokens = std::vector<std::string>;

TEST_CASE("split_command_line: the documented CommandLineToArgvW examples") {
    CHECK(split_command_line(R"("abc" d e)") == Tokens{"abc", "d", "e"});
    CHECK(split_command_line(R"(a\\\b d"e f"g h)") == Tokens{R"(a\\\b)", "de fg", "h"});
    CHECK(split_command_line(R"(a\\\"b c d)") == Tokens{R"(a\"b)", "c", "d"});
    CHECK(split_command_line(R"(a\\\\"b c" d e)") == Tokens{R"(a\\b c)", "d", "e"});
}

TEST_CASE("split_command_line: empty arguments, doubled quotes, whitespace") {
    CHECK(split_command_line("").empty());
    CHECK(split_command_line("   \t ").empty());
    CHECK(split_command_line(R"(a "" b)") == Tokens{"a", "", "b"});
    CHECK(split_command_line(R"("a ""b"" c")") == Tokens{R"(a "b" c)"});
    CHECK(split_command_line("a\r\nb\tc") == Tokens{"a", "b", "c"});
    CHECK(split_command_line(R"(/Fo"x\y\\" next)") == Tokens{R"(/Fox\y\)", "next"});
    CHECK(split_command_line(R"("unterminated quote)") == Tokens{"unterminated quote"});
}

TEST_CASE("quote_argument / join_command_line round-trip") {
    const Tokens samples = {"plain",          "with space",     "",         R"(has"quote)",     R"(trailing\)",
                            R"(trailing\\)",  R"(a\"b)",        R"(a\\"b)", R"(C:\Program Files\x\)", "tab\there",
                            R"(/DNAME="a b")", R"(\\server\share\dir with space\)", "/Fo<OUT>", "--arg=value"};
    for (const std::string& s : samples) {
        const std::string quoted = quote_argument(s);
        CAPTURE(s);
        CAPTURE(quoted);
        REQUIRE(split_command_line(quoted) == Tokens{s});
    }
    CHECK(join_command_line(samples) != "");
    CHECK(split_command_line(join_command_line(samples)) == samples);
    CHECK(quote_argument("plain") == "plain");
    CHECK(quote_argument("with space") == R"("with space")");
    CHECK(quote_argument("") == R"("")");
}

TEST_CASE("split_command_line_posix: clang's CodeView quoting") {
    CHECK(split_command_line_posix(R"("-cc1" "-triple" "x86_64-pc-windows-msvc19.44.0")") ==
          Tokens{"-cc1", "-triple", "x86_64-pc-windows-msvc19.44.0"});
    // llvm::sys::printArg escapes backslashes: "D:\\proj" is D:\proj.
    CHECK(split_command_line_posix(R"("-fdebug-compilation-dir=D:\\proj\\build" "-I" "C:\\Program Files\\x")") ==
          Tokens{R"(-fdebug-compilation-dir=D:\proj\build)", "-I", R"(C:\Program Files\x)"});
    CHECK(split_command_line_posix(R"("-D" "NAME=\"a b\"" "-D" "COST=\$5")") == Tokens{"-D", R"(NAME="a b")", "-D", "COST=$5"});
    // A backslash before any other character stays.
    CHECK(split_command_line_posix(R"("a\b")") == Tokens{R"(a\b)"});
    CHECK(split_command_line_posix(R"("")") == Tokens{""});
}

TEST_CASE("decode_response_file_bytes") {
    CHECK(decode_response_file_bytes("plain") == "plain");
    CHECK(decode_response_file_bytes("\xEF\xBB\xBF/c /Zi") == "/c /Zi");
    // UTF-16LE with BOM: "/c" then U+00E9 then an astral character (surrogate pair U+1F600).
    const std::string utf16("\xFF\xFE/\0c\0\xE9\0\x3D\xD8\x00\xDE", 12);
    CHECK(decode_response_file_bytes(utf16) == "/c\xC3\xA9\xF0\x9F\x98\x80");
}

namespace {
struct FakeFs {
    std::map<std::string, std::string> files;
    FileReader reader() {
        return [this](const std::string& p) -> cml::Result<std::string> {
            const auto it = files.find(p);
            if (it == files.end()) {
                return cml::Diag::make(diag::kResponseFileUnreadable, "missing " + p, "x");
            }
            return decode_response_file_bytes(it->second);
        };
    }
};
}  // namespace

TEST_CASE("expand_response_files") {
    FakeFs fs;
    fs.files[R"(D:\w\a.rsp)"] = "/DA=1 @b.rsp\n/DA2";
    fs.files[R"(D:\w\b.rsp)"] = "\xEF\xBB\xBF/DB=\"x y\"";
    fs.files[R"(D:\other\abs.rsp)"] = "/DABS";
    fs.files[R"(D:\w\self.rsp)"] = "@self.rsp";

    SUBCASE("relative, nested, absolute, quoted") {
        auto r = expand_response_files({"/c", "@a.rsp", R"(@"D:\other\abs.rsp")", "/Zi"}, R"(D:\w)", fs.reader());
        REQUIRE(r);
        CHECK(*r == Tokens{"/c", "/DA=1", R"(/DB=x y)", "/DA2", "/DABS", "/Zi"});
    }
    SUBCASE("a lone @ and @-less tokens stay") {
        auto r = expand_response_files({"@", "/D@x", "file@name"}, R"(D:\w)", fs.reader());
        REQUIRE(r);
        CHECK(*r == Tokens{"@", "/D@x", "file@name"});
    }
    SUBCASE("missing file is CML1500") {
        auto r = expand_response_files({"@nope.rsp"}, R"(D:\w)", fs.reader());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == diag::kResponseFileUnreadable);
    }
    SUBCASE("a cycle is CML1501") {
        auto r = expand_response_files({"@self.rsp"}, R"(D:\w)", fs.reader());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == diag::kResponseFileTooDeep);
        CHECK(r.error().valid());
    }
}

TEST_CASE("default_file_reader reports an unreadable file as CML1500") {
    auto r = default_file_reader()("/definitely/not/here.rsp");
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kResponseFileUnreadable);
}
