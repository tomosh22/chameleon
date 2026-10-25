#pragma once

// Windows command-line tokenisation, quoting and response files (WP-05, plan section 4.4).
//
// The tokeniser follows CommandLineToArgvW / the UCRT's parse_cmdline for everything after the
// program name: whitespace separates arguments outside quotes; 2n backslashes before a quote give
// n backslashes and toggle quoting; 2n+1 backslashes before a quote give n backslashes and a
// literal quote; inside quotes a doubled quote ("") is one literal quote. cl.exe and
// clang(-cl).exe use the same rules, so a recorded command line round-trips through
// split_command_line() / join_command_line().
//
// Response files: cl's "@file" (also understood by clang-cl). Contents are tokenised with the
// same rules, newlines count as whitespace, nested "@file" tokens are expanded (depth-limited),
// UTF-16LE / UTF-8 byte-order marks are honoured. Relative paths are relative to the recorded cwd.

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cml/util/result.h"

namespace cml::toolchain {

// What a doubled quote inside a quoted region means. The two Windows parsers differ:
//   Crt        the C runtime's argv parser since VS2008 / the UCRT (parse_cmdline): `""` inside quotes is one literal
//              `"` and the region stays quoted. cl.exe and clang(-cl) parse their command line this way, so it is the
//              default and what every recorded command is split with.
//   ShellArgv  shell32's CommandLineToArgvW: `""` inside quotes emits one `"` and ENDS the quoted region.
// Everything else (whitespace, backslash runs before quotes) is identical in both.
enum class QuoteMode { Crt, ShellArgv };

// Splits `command_line` into arguments. There is no special first-argument rule: callers decide
// whether the first token is a program name (see CompileRecord).
std::vector<std::string> split_command_line(std::string_view command_line, QuoteMode mode = QuoteMode::Crt);

// The quoting clang uses when it records its cc1 command line in CodeView (llvm::sys::printArg
// with Quote=true): every argument in double quotes, and inside them a backslash escapes `"`, `\`,
// `$` and the backtick; any other backslash is literal. Differs from the Windows rules for paths:
// "D:\\proj" is D:\proj here and D:\\proj under CommandLineToArgvW.
std::vector<std::string> split_command_line_posix(std::string_view command_line);

// Quotes one argument so that split_command_line() returns it unchanged ("" for an empty one).
std::string quote_argument(std::string_view argument);
std::string join_command_line(std::span<const std::string> arguments);

// Reads a response file; the result is UTF-8 text. `path` is already resolved against the cwd.
using FileReader = std::function<Result<std::string>(const std::string& path)>;
// Reads from disk and decodes the byte-order mark. CML1500 when the file cannot be read.
FileReader default_file_reader();
// UTF-16LE (BOM FF FE) and UTF-8 (BOM EF BB BF) are decoded to UTF-8; anything else is returned
// as is (the ANSI code page is not converted).
std::string decode_response_file_bytes(std::string_view bytes);

inline constexpr int kMaxResponseFileDepth = 8;
inline constexpr int kMaxResponseFiles = 256;               // files read by one expansion (a file included twice counts twice)
inline constexpr std::size_t kMaxResponseTokens = 1000000;  // tokens produced by one expansion

// Replaces every "@file" token by the tokens of that file (recursively). A token that is just "@"
// stays. CML1500 for an unreadable file, CML1501 beyond kMaxResponseFileDepth (also catches cycles).
Result<std::vector<std::string>> expand_response_files(const std::vector<std::string>& arguments,
                                                       std::string_view cwd, const FileReader& reader);

}  // namespace cml::toolchain
