#include "cml/toolchain/compile_commands.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <utility>

#include "cml/pdb/index.h"
#include "cml/toolchain/command_line.h"
#include "cml/toolchain/diag_ids.h"
#include "fs_util.h"

namespace cml::toolchain {

namespace {

// ---- minimal JSON DOM -----------------------------------------------------------------------
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    std::string str;                                    // String (UTF-8)
    std::vector<Json> items;                            // Array
    std::vector<std::string> keys;                      // Object: keys and values in document order
    std::vector<Json> values;

    const Json* find(std::string_view key) const {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                return &values[i];
            }
        }
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Result<Json> parse_document() {
        skip_ws();
        // UTF-8 BOM
        Json root;
        Result<void> r = parse_value(root, 0);
        if (!r) {
            return r.error();
        }
        skip_ws();
        if (pos_ != s_.size()) {
            return fail("unexpected data after the top-level value");
        }
        return root;
    }

private:
    Diag fail(const std::string& what) const {
        return Diag::make(diag::kBadJson, "compile_commands.json is malformed at byte " + std::to_string(pos_) + ": " + what,
                          "Regenerate compile_commands.json (CMake: -DCMAKE_EXPORT_COMPILE_COMMANDS=ON).");
    }

    bool at_end() const { return pos_ >= s_.size(); }
    char peek() const { return s_[pos_]; }

    void skip_ws() {
        if (pos_ == 0 && s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF &&
            static_cast<unsigned char>(s_[1]) == 0xBB && static_cast<unsigned char>(s_[2]) == 0xBF) {
            pos_ = 3;
        }
        while (!at_end() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            ++pos_;
        }
    }

    bool consume_literal(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) == lit) {
            pos_ += lit.size();
            return true;
        }
        return false;
    }

    Result<void> parse_value(Json& out, int depth) {
        if (depth > kMaxJsonDepth) {
            return fail("nesting is deeper than " + std::to_string(kMaxJsonDepth));
        }
        skip_ws();
        if (at_end()) {
            return fail("unexpected end of input");
        }
        const char c = peek();
        if (c == '{') {
            return parse_object(out, depth);
        }
        if (c == '[') {
            return parse_array(out, depth);
        }
        if (c == '"') {
            out.type = Json::Type::String;
            return parse_string(out.str);
        }
        if (c == 't') {
            out.type = Json::Type::Bool;
            return consume_literal("true") ? Result<void>() : Result<void>(fail("bad literal"));
        }
        if (c == 'f') {
            out.type = Json::Type::Bool;
            return consume_literal("false") ? Result<void>() : Result<void>(fail("bad literal"));
        }
        if (c == 'n') {
            out.type = Json::Type::Null;
            return consume_literal("null") ? Result<void>() : Result<void>(fail("bad literal"));
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(out);
        }
        return fail(std::string("unexpected character '") + c + "'");
    }

    Result<void> parse_number(Json& out) {
        out.type = Json::Type::Number;
        const std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        const auto digits = [&]() {
            const std::size_t b = pos_;
            while (!at_end() && peek() >= '0' && peek() <= '9') {
                ++pos_;
            }
            return pos_ > b;
        };
        if (!digits()) {
            return fail("bad number");
        }
        if (!at_end() && peek() == '.') {
            ++pos_;
            if (!digits()) {
                return fail("bad number");
            }
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!at_end() && (peek() == '+' || peek() == '-')) {
                ++pos_;
            }
            if (!digits()) {
                return fail("bad number");
            }
        }
        out.str.assign(s_.substr(start, pos_ - start));
        return {};
    }

    static int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool read_hex4(std::uint32_t& value) {
        if (pos_ + 4 > s_.size()) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hex_value(s_[pos_ + static_cast<std::size_t>(i)]);
            if (h < 0) {
                return false;
            }
            value = value * 16 + static_cast<std::uint32_t>(h);
        }
        pos_ += 4;
        return true;
    }

    static void put_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    Result<void> parse_string(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        while (true) {
            if (at_end()) {
                return fail("unterminated string");
            }
            const char c = s_[pos_++];
            if (c == '"') {
                return {};
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("control character in string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at_end()) {
                return fail("unterminated escape");
            }
            const char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!read_hex4(cp)) {
                        return fail("bad \\u escape");
                    }
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        std::uint32_t low = 0;
                        if (s_.substr(pos_, 2) != "\\u") {
                            return fail("unpaired surrogate");
                        }
                        pos_ += 2;
                        if (!read_hex4(low) || low < 0xDC00 || low >= 0xE000) {
                            return fail("unpaired surrogate");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        return fail("unpaired surrogate");
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
    }

    Result<void> parse_array(Json& out, int depth) {
        out.type = Json::Type::Array;
        ++pos_;
        skip_ws();
        if (!at_end() && peek() == ']') {
            ++pos_;
            return {};
        }
        while (true) {
            out.items.emplace_back();
            Result<void> r = parse_value(out.items.back(), depth + 1);
            if (!r) {
                return r;
            }
            skip_ws();
            if (at_end()) {
                return fail("unterminated array");
            }
            const char c = s_[pos_++];
            if (c == ']') {
                return {};
            }
            if (c != ',') {
                return fail("expected ',' or ']'");
            }
        }
    }

    Result<void> parse_object(Json& out, int depth) {
        out.type = Json::Type::Object;
        ++pos_;
        skip_ws();
        if (!at_end() && peek() == '}') {
            ++pos_;
            return {};
        }
        while (true) {
            skip_ws();
            if (at_end() || peek() != '"') {
                return fail("expected a string key");
            }
            std::string key;
            Result<void> kr = parse_string(key);
            if (!kr) {
                return kr;
            }
            skip_ws();
            if (at_end() || s_[pos_] != ':') {
                return fail("expected ':'");
            }
            ++pos_;
            out.keys.push_back(std::move(key));
            out.values.emplace_back();
            Result<void> r = parse_value(out.values.back(), depth + 1);
            if (!r) {
                return r;
            }
            skip_ws();
            if (at_end()) {
                return fail("unterminated object");
            }
            const char c = s_[pos_++];
            if (c == '}') {
                return {};
            }
            if (c != ',') {
                return fail("expected ',' or '}'");
            }
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

std::string string_member(const Json& obj, std::string_view key) {
    const Json* v = obj.find(key);
    return (v && v->type == Json::Type::String) ? v->str : std::string();
}

}  // namespace

Result<std::vector<CompileCommandEntry>> parse_compile_commands(std::string_view json) {
    if (json.size() > kMaxJsonBytes) {
        return Diag::make(diag::kBadJson, "compile_commands.json is larger than " + std::to_string(kMaxJsonBytes) + " bytes",
                          "Split the project or generate a smaller compilation database.");
    }
    Parser parser(json);
    Result<Json> doc = parser.parse_document();
    if (!doc) {
        return doc.error();
    }
    if (doc->type != Json::Type::Array) {
        return Diag::make(diag::kBadJson, "compile_commands.json: the top-level value is not an array",
                          "Regenerate compile_commands.json (CMake: -DCMAKE_EXPORT_COMPILE_COMMANDS=ON).");
    }
    std::vector<CompileCommandEntry> out;
    out.reserve(doc->items.size());
    for (const Json& item : doc->items) {
        if (item.type != Json::Type::Object) {
            return Diag::make(diag::kBadJson, "compile_commands.json: an entry is not an object",
                              "Regenerate compile_commands.json (CMake: -DCMAKE_EXPORT_COMPILE_COMMANDS=ON).");
        }
        CompileCommandEntry e;
        e.directory = string_member(item, "directory");
        e.file = string_member(item, "file");
        e.output = string_member(item, "output");
        e.command = string_member(item, "command");
        if (const Json* args = item.find("arguments"); args && args->type == Json::Type::Array) {
            e.has_arguments = true;
            for (const Json& a : args->items) {
                if (a.type == Json::Type::String) {
                    e.arguments.push_back(a.str);
                }
            }
        }
        if (e.file.empty()) {
            continue;
        }
        out.push_back(std::move(e));
    }
    return out;
}

Result<std::vector<CompileCommandEntry>> load_compile_commands(const std::string& path) {
    std::ifstream in(detail::to_path(path), std::ios::binary);
    if (!in) {
        return Diag::make(diag::kCompileCommandsUnreadable, "cannot read '" + path + "'",
                          "Check the compile_commands.json path (CMake: -DCMAKE_EXPORT_COMPILE_COMMANDS=ON).");
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_compile_commands(text);
}

const CompileCommandEntry* find_compile_command(const std::vector<CompileCommandEntry>& entries,
                                                std::string_view source, std::string_view cwd_hint) {
    const std::string want = pdb::normalize_path(source, cwd_hint);
    for (const CompileCommandEntry& e : entries) {
        if (pdb::normalize_path(e.file, e.directory) == want) {
            return &e;
        }
    }
    return nullptr;
}

Result<CompileRecord> record_from_compile_command(const CompileCommandEntry& entry) {
    std::vector<std::string> tokens = entry.has_arguments ? entry.arguments : split_command_line(entry.command);
    if (tokens.empty()) {
        return Diag::make(diag::kNoCompileCommandsEntry, "compile_commands.json entry for '" + entry.file + "' has no command",
                          "Regenerate compile_commands.json.");
    }
    CompileRecord r;
    r.origin = RecordOrigin::CompileCommands;
    r.cwd = entry.directory;
    r.compiler = tokens.front();
    r.args.assign(tokens.begin() + 1, tokens.end());
    r.source = detail::is_absolute(entry.file) || entry.directory.empty()
                   ? entry.file
                   : entry.directory + (entry.directory.back() == '\\' || entry.directory.back() == '/' ? "" : "\\") + entry.file;
    return r;
}

Result<CompileRecord> record_from_compile_commands_file(const std::string& compile_commands_path, std::string_view source) {
    Result<std::vector<CompileCommandEntry>> entries = load_compile_commands(compile_commands_path);
    if (!entries) {
        return entries.error();
    }
    const CompileCommandEntry* e = find_compile_command(*entries, source);
    if (e == nullptr) {
        return Diag::make(diag::kNoCompileCommandsEntry,
                          "'" + std::string(source) + "' has no entry in " + compile_commands_path,
                          "Add the file to the build so that compile_commands.json lists it, then regenerate it.");
    }
    return record_from_compile_command(*e);
}

}  // namespace cml::toolchain
