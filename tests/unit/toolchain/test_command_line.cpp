// Tokenisation, quoting and response files (WP-05).
#include "cml/toolchain/command_line.h"
#include "cml/toolchain/diag_ids.h"
#include "cml/toolchain/record.h"

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

TEST_CASE("tokenize_recorded_command: Windows rules for cl, clang quoting for cc1") {
    CHECK(tokenize_recorded_command(R"(-c -I"C:\Program Files\VS\include" -X)") == Tokens{"-c", R"(-IC:\Program Files\VS\include)", "-X"});
    CHECK(tokenize_recorded_command(R"("-cc1" "-I" "C:\\Program Files\\x")") == Tokens{"-cc1", "-I", R"(C:\Program Files\x)"});
    CHECK(tokenize_recorded_command(R"(-cc1 "-I" "a")") == Tokens{"-cc1", "-I", "a"});
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

TEST_CASE("quote modes: the CRT parser (cl) and CommandLineToArgvW differ on a doubled quote inside quotes") {
    // CRT (default, what cl.exe and clang see): "" inside quotes is one literal quote, the region stays quoted.
    CHECK(split_command_line(R"("a ""b"" c")") == Tokens{R"(a "b" c)"});
    CHECK(split_command_line(R"("a ""b"" c")", QuoteMode::Crt) == Tokens{R"(a "b" c)"});
    // CommandLineToArgvW: "" inside quotes emits one quote and ENDS the quoted region, so ` c` is split off.
    CHECK(split_command_line(R"("a ""b"" c")", QuoteMode::ShellArgv) == Tokens{R"(a "b)", "c"});
    CHECK(split_command_line(R"(x"y ""z)", QuoteMode::Crt) == Tokens{R"(xy "z)"});
    CHECK(split_command_line(R"(x"y ""z w)", QuoteMode::ShellArgv) == Tokens{R"(xy "z)", "w"});
    // Everything else is identical.
    for (const char* text : {R"(a\\\"b c d)", R"("abc" d e)", R"(a\\\\"b c" d e)", "a b\tc", R"("" x)"}) {
        CAPTURE(text);
        CHECK(split_command_line(text, QuoteMode::Crt) == split_command_line(text, QuoteMode::ShellArgv));
    }
}

TEST_CASE("response files: real cycles, limits") {
    FakeFs fs;
    SUBCASE("a cycle through two files is CML1501, a diamond (same file twice, not nested) is fine") {
        fs.files[R"(D:\w\a.rsp)"] = "@b.rsp";
        fs.files[R"(D:\w\b.rsp)"] = "@A.RSP";  // case-insensitive path, same file
        auto cyc = expand_response_files({"@a.rsp"}, R"(D:\w)", fs.reader());
        REQUIRE_FALSE(cyc);
        CHECK(cyc.error().code == diag::kResponseFileTooDeep);
        CHECK(cyc.error().message.find("cycle") != std::string::npos);

        fs.files[R"(D:\w\c.rsp)"] = "@leaf.rsp @leaf.rsp /DX";
        fs.files[R"(D:\w\leaf.rsp)"] = "/DLEAF";
        auto diamond = expand_response_files({"@c.rsp", "@leaf.rsp"}, R"(D:\w)", fs.reader());
        REQUIRE(diamond);
        CHECK(*diamond == Tokens{"/DLEAF", "/DLEAF", "/DX", "/DLEAF"});
    }
    SUBCASE("a file that includes itself directly") {
        fs.files[R"(D:\w\s.rsp)"] = "/DA @s.rsp";
        auto r = expand_response_files({"@s.rsp"}, R"(D:\w)", fs.reader());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == diag::kResponseFileTooDeep);
    }
    SUBCASE("more than 256 files read") {
        fs.files[R"(D:\w\leaf.rsp)"] = "/DL";
        Tokens args;
        for (int i = 0; i < kMaxResponseFiles + 1; ++i) {
            args.push_back("@leaf.rsp");
        }
        auto r = expand_response_files(args, R"(D:\w)", fs.reader());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == diag::kResponseFileTooDeep);
        args.pop_back();
        auto ok = expand_response_files(args, R"(D:\w)", fs.reader());
        REQUIRE(ok);
        CHECK(ok->size() == static_cast<std::size_t>(kMaxResponseFiles));
    }
    SUBCASE("more than 1,000,000 tokens") {
        std::string big;
        big.reserve(2 * 600000);
        for (int i = 0; i < 600000; ++i) {
            big += "x ";
        }
        fs.files[R"(D:\w\big.rsp)"] = big;
        auto one = expand_response_files({"@big.rsp"}, R"(D:\w)", fs.reader());
        REQUIRE(one);
        CHECK(one->size() == 600000);
        auto two = expand_response_files({"@big.rsp", "@big.rsp"}, R"(D:\w)", fs.reader());
        REQUIRE_FALSE(two);
        CHECK(two.error().code == diag::kResponseFileTooDeep);
        // Plain arguments count too.
        Tokens many(kMaxResponseTokens + 1, "y");
        auto plain = expand_response_files(many, R"(D:\w)", fs.reader());
        REQUIRE_FALSE(plain);
    }
}
