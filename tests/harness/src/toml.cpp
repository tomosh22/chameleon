#include "toml.h"

#include <algorithm>
#include <charconv>
#include <system_error>

#include "cml/ipc/control_ids.h"

namespace cmltest::toml {

const Value* Table::find(std::string_view key) const {
    for (const auto& entry : entries) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

const Table* Document::find_table(std::string_view name) const {
    for (const Table& t : tables) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

namespace {

cml::Diag syntax(int line, const std::string& what) {
    return cml::Diag::make(cml::ids::kScenarioSyntax, "scenario syntax error at line " + std::to_string(line) + ": " + what,
                           "Fix the line; scenario files use a small TOML subset (see tests/harness/README.md).");
}

bool is_bare_key_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

void append_utf8(std::string* s, std::uint32_t cp) {
    if (cp < 0x80) {
        s->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Parses one logical line (already stripped of its line terminator).
class LineParser {
public:
    LineParser(std::string_view line, int number) : line_(line), number_(number) {}

    void skip_blanks() {
        while (pos_ < line_.size() && (line_[pos_] == ' ' || line_[pos_] == '\t')) {
            ++pos_;
        }
    }
    bool at_end() const { return pos_ >= line_.size(); }
    char peek() const { return line_[pos_]; }
    void advance() { ++pos_; }
    std::size_t pos() const { return pos_; }

    // Only blanks and a comment may follow.
    cml::Result<void> expect_end_of_line() {
        skip_blanks();
        if (!at_end() && peek() != '#') {
            return syntax(number_, "unexpected text '" + std::string(line_.substr(pos_, 20)) + "' after the value");
        }
        return cml::Result<void>();
    }

    cml::Result<std::string> bare_key() {
        const std::size_t start = pos_;
        while (pos_ < line_.size() && is_bare_key_char(line_[pos_])) {
            ++pos_;
        }
        if (pos_ == start) {
            return syntax(number_, "a bare key (letters, digits, '_' or '-') was expected");
        }
        return std::string(line_.substr(start, pos_ - start));
    }

    cml::Result<Value> value() {
        Value v;
        v.line = number_;
        if (at_end()) {
            return syntax(number_, "a value was expected after '='");
        }
        const char c = peek();
        if (c == '"') {
            if (line_.substr(pos_, 3) == "\"\"\"") {
                return syntax(number_, "multi-line strings are not supported");
            }
            cml::Result<std::string> s = basic_string();
            if (!s) {
                return s.error();
            }
            v.kind = Value::Kind::String;
            v.text = std::move(*s);
            return v;
        }
        if (c == '\'') {
            if (line_.substr(pos_, 3) == "'''") {
                return syntax(number_, "multi-line strings are not supported");
            }
            ++pos_;
            const std::size_t end = line_.find('\'', pos_);
            if (end == std::string_view::npos) {
                return syntax(number_, "unterminated literal string");
            }
            v.kind = Value::Kind::String;
            v.text = std::string(line_.substr(pos_, end - pos_));
            pos_ = end + 1;
            return v;
        }
        if (c == '[' || c == '{') {
            return syntax(number_, "arrays of values and inline tables are not supported");
        }
        // bare token: boolean or integer
        const std::size_t start = pos_;
        while (pos_ < line_.size() && line_[pos_] != ' ' && line_[pos_] != '\t' && line_[pos_] != '#') {
            ++pos_;
        }
        const std::string token(line_.substr(start, pos_ - start));
        if (token == "true" || token == "false") {
            v.kind = Value::Kind::Boolean;
            v.boolean = token == "true";
            return v;
        }
        std::string digits;
        std::size_t i = 0;
        if (i < token.size() && (token[i] == '+' || token[i] == '-')) {
            digits.push_back(token[i++]);
        }
        const std::size_t first_digit = i;
        bool ok = i < token.size();
        for (; i < token.size(); ++i) {
            if (token[i] >= '0' && token[i] <= '9') {
                digits.push_back(token[i]);
            } else if (token[i] == '_' && i > first_digit && i + 1 < token.size() && token[i + 1] != '_') {
                continue;
            } else {
                ok = false;
                break;
            }
        }
        if (ok) {
            const char* first = digits.data() + (digits[0] == '+' ? 1 : 0);
            const char* last = digits.data() + digits.size();
            std::int64_t number = 0;
            const std::from_chars_result r = std::from_chars(first, last, number);
            if (r.ec == std::errc() && r.ptr == last) {
                v.kind = Value::Kind::Integer;
                v.integer = number;
                return v;
            }
            return syntax(number_, "integer '" + token + "' is out of range");
        }
        return syntax(number_, "unsupported value '" + token + "' (use a quoted string, a decimal integer, true or false)");
    }

private:
    cml::Result<std::string> basic_string() {
        ++pos_;  // opening quote
        std::string out;
        for (;;) {
            if (pos_ >= line_.size()) {
                return syntax(number_, "unterminated string");
            }
            const char c = line_[pos_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
                return syntax(number_, "control character in string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= line_.size()) {
                return syntax(number_, "unterminated escape");
            }
            const char e = line_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u':
                case 'U': {
                    const std::size_t digits = e == 'u' ? 4 : 8;
                    if (pos_ + digits > line_.size()) {
                        return syntax(number_, "truncated \\u escape");
                    }
                    std::uint32_t cp = 0;
                    for (std::size_t i = 0; i < digits; ++i) {
                        const char h = line_[pos_ + i];
                        int d = -1;
                        if (h >= '0' && h <= '9') d = h - '0';
                        else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
                        if (d < 0) {
                            return syntax(number_, "invalid hex digit in \\u escape");
                        }
                        cp = cp * 16 + static_cast<std::uint32_t>(d);
                    }
                    pos_ += digits;
                    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                        return syntax(number_, "\\u escape is not a Unicode scalar value");
                    }
                    append_utf8(&out, cp);
                    break;
                }
                default:
                    return syntax(number_, std::string("unknown escape '\\") + e + "'");
            }
        }
    }

    std::string_view line_;
    int number_;
    std::size_t pos_ = 0;
};

}  // namespace

cml::Result<Document> parse(std::string_view text) {
    if (text.size() > kMaxDocumentBytes) {
        return syntax(1, "the file is larger than 1 MiB");
    }
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);
    }
    Document doc;
    // Which table receives key/value lines: the root, a [table] or the last [[array table]].
    enum class Target { Root, Table, ArrayTable };
    Target target = Target::Root;
    std::size_t target_index = 0;

    int number = 0;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        if (offset == text.size()) {
            break;
        }
        std::size_t end = text.find('\n', offset);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(offset, end - offset);
        offset = end + 1;
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.size() > kMaxLineBytes) {
            return syntax(number, "line is longer than 64 KiB");
        }
        if (std::find(line.begin(), line.end(), '\0') != line.end()) {
            return syntax(number, "NUL character in line");
        }
        LineParser p(line, number);
        p.skip_blanks();
        if (p.at_end() || p.peek() == '#') {
            continue;
        }
        if (p.peek() == '[') {
            p.advance();
            const bool array = !p.at_end() && p.peek() == '[';
            if (array) {
                p.advance();
            }
            p.skip_blanks();
            cml::Result<std::string> name = p.bare_key();
            if (!name) {
                return name.error();
            }
            p.skip_blanks();
            if (!p.at_end() && p.peek() == '.') {
                return syntax(number, "dotted table names are not supported");
            }
            for (int closing = array ? 2 : 1; closing > 0; --closing) {
                if (p.at_end() || p.peek() != ']') {
                    return syntax(number, array ? "']]' expected after the table name" : "']' expected after the table name");
                }
                p.advance();
            }
            cml::Result<void> rest = p.expect_end_of_line();
            if (!rest) {
                return rest.error();
            }
            if (doc.tables.size() + doc.array_tables.size() >= kMaxTables) {
                return syntax(number, "more than 4096 tables");
            }
            Table table;
            table.name = *name;
            table.line = number;
            if (array) {
                if (doc.find_table(*name) != nullptr) {
                    return syntax(number, "[[" + *name + "]] conflicts with the table [" + *name + "] defined earlier");
                }
                doc.array_tables.push_back(std::move(table));
                target = Target::ArrayTable;
                target_index = doc.array_tables.size() - 1;
            } else {
                if (doc.find_table(*name) != nullptr) {
                    return syntax(number, "table [" + *name + "] is defined twice");
                }
                for (const Table& t : doc.array_tables) {
                    if (t.name == *name) {
                        return syntax(number, "[" + *name + "] conflicts with the array of tables [[" + *name + "]]");
                    }
                }
                doc.tables.push_back(std::move(table));
                target = Target::Table;
                target_index = doc.tables.size() - 1;
            }
            continue;
        }
        // key = value
        cml::Result<std::string> key = p.bare_key();
        if (!key) {
            if (!p.at_end() && (p.peek() == '"' || p.peek() == '\'')) {
                return syntax(number, "quoted keys are not supported");
            }
            return key.error();
        }
        p.skip_blanks();
        if (!p.at_end() && p.peek() == '.') {
            return syntax(number, "dotted keys are not supported");
        }
        if (p.at_end() || p.peek() != '=') {
            return syntax(number, "'=' expected after key '" + *key + "'");
        }
        p.advance();
        p.skip_blanks();
        cml::Result<Value> value = p.value();
        if (!value) {
            return value.error();
        }
        cml::Result<void> rest = p.expect_end_of_line();
        if (!rest) {
            return rest.error();
        }
        Table& table = target == Target::Root ? doc.root
                       : target == Target::Table ? doc.tables[target_index]
                                                 : doc.array_tables[target_index];
        if (table.find(*key) != nullptr) {
            return syntax(number, "duplicate key '" + *key + "'");
        }
        if (table.entries.size() >= kMaxKeysPerTable) {
            return syntax(number, "more than 1024 keys in one table");
        }
        table.entries.emplace_back(std::move(*key), std::move(*value));
    }
    return doc;
}

}  // namespace cmltest::toml
