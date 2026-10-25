#include "cml/ipc/json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

#include "cml/ipc/control_ids.h"
#include "cml/ipc/messages.h"

namespace cml::ipc::json {

// --- Value --------------------------------------------------------------------------------------

Value Value::boolean(bool v) {
    Value out;
    out.type_ = Type::Bool;
    out.bool_ = v;
    return out;
}

Value Value::integer(std::int64_t v) {
    Value out;
    out.type_ = Type::Integer;
    out.int_ = v;
    return out;
}

Value Value::number(double v) {
    Value out;
    out.type_ = Type::Double;
    out.double_ = v;
    return out;
}

Value Value::string(std::string v) {
    Value out;
    out.type_ = Type::String;
    out.string_ = std::move(v);
    return out;
}

Value Value::array() {
    Value out;
    out.type_ = Type::Array;
    return out;
}

Value Value::object() {
    Value out;
    out.type_ = Type::Object;
    return out;
}

bool Value::as_bool(bool fallback) const noexcept {
    return type_ == Type::Bool ? bool_ : fallback;
}

std::int64_t Value::as_integer(std::int64_t fallback) const noexcept {
    return type_ == Type::Integer ? int_ : fallback;
}

double Value::as_double(double fallback) const noexcept {
    if (type_ == Type::Double) {
        return double_;
    }
    if (type_ == Type::Integer) {
        return static_cast<double>(int_);
    }
    return fallback;
}

const std::string& Value::as_string() const noexcept {
    static const std::string kEmpty;
    return type_ == Type::String ? string_ : kEmpty;
}

Value& Value::push(Value v) {
    if (type_ != Type::Array) {
        return *this;
    }
    values_.push_back(std::move(v));
    return values_.back();
}

const Value* Value::find(std::string_view key) const noexcept {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            return &values_[i];
        }
    }
    return nullptr;
}

Value& Value::set(std::string key, Value v) {
    if (type_ != Type::Object) {
        return *this;
    }
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            values_[i] = std::move(v);
            return values_[i];
        }
    }
    keys_.push_back(std::move(key));
    values_.push_back(std::move(v));
    return values_.back();
}

std::string Value::get_string(std::string_view key, const std::string& fallback) const {
    const Value* v = find(key);
    return v != nullptr && v->is_string() ? v->as_string() : fallback;
}

std::int64_t Value::get_integer(std::string_view key, std::int64_t fallback) const noexcept {
    const Value* v = find(key);
    return v != nullptr && v->is_integer() ? v->as_integer() : fallback;
}

bool Value::get_bool(std::string_view key, bool fallback) const noexcept {
    const Value* v = find(key);
    return v != nullptr && v->is_bool() ? v->as_bool() : fallback;
}

// --- parser -------------------------------------------------------------------------------------

namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Result<Value> run() {
        if (text_.size() > kMaxDocumentBytes) {
            return fail(0, "document is larger than 1 MiB");
        }
        if (!is_valid_utf8(text_)) {
            return fail(0, "document is not valid UTF-8");
        }
        skip_space();
        Value value;
        if (!parse_value(&value, 0)) {
            return *error_;
        }
        skip_space();
        if (pos_ != text_.size()) {
            return fail(pos_, "unexpected data after the JSON value");
        }
        return value;
    }

private:
    Diag make_error(std::size_t offset, const std::string& what) const {
        return Diag::make(ids::kControlJsonMalformed,
                          "malformed JSON at byte " + std::to_string(offset) + ": " + what,
                          "Send one complete JSON object per line (see docs/wp/WP-13.md, Control protocol).");
    }

    Diag fail(std::size_t offset, const std::string& what) {
        error_ = make_error(offset, what);
        return *error_;
    }

    bool set_error(std::size_t offset, const std::string& what) {
        if (!error_) {
            error_ = make_error(offset, what);
        }
        return false;
    }

    void skip_space() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume_literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) == word) {
            pos_ += word.size();
            return true;
        }
        return false;
    }

    bool parse_value(Value* out, std::size_t depth) {
        if (pos_ >= text_.size()) {
            return set_error(pos_, "unexpected end of input");
        }
        const char c = text_[pos_];
        switch (c) {
            case '{':
                return parse_object(out, depth);
            case '[':
                return parse_array(out, depth);
            case '"': {
                std::string s;
                if (!parse_string(&s)) {
                    return false;
                }
                *out = Value::string(std::move(s));
                return true;
            }
            case 't':
                if (consume_literal("true")) {
                    *out = Value::boolean(true);
                    return true;
                }
                return set_error(pos_, "invalid literal");
            case 'f':
                if (consume_literal("false")) {
                    *out = Value::boolean(false);
                    return true;
                }
                return set_error(pos_, "invalid literal");
            case 'n':
                if (consume_literal("null")) {
                    *out = Value();
                    return true;
                }
                return set_error(pos_, "invalid literal");
            default:
                if (c == '-' || (c >= '0' && c <= '9')) {
                    return parse_number(out);
                }
                return set_error(pos_, "unexpected character");
        }
    }

    static bool is_digit(char c) { return c >= '0' && c <= '9'; }

    bool parse_number(Value* out) {
        const std::size_t start = pos_;
        std::size_t p = pos_;
        if (p < text_.size() && text_[p] == '-') {
            ++p;
        }
        if (p >= text_.size() || !is_digit(text_[p])) {
            return set_error(p, "digit expected in number");
        }
        if (text_[p] == '0') {
            ++p;
            if (p < text_.size() && is_digit(text_[p])) {
                return set_error(p, "leading zero in number");
            }
        } else {
            while (p < text_.size() && is_digit(text_[p])) {
                ++p;
            }
        }
        bool integral = true;
        if (p < text_.size() && text_[p] == '.') {
            integral = false;
            ++p;
            if (p >= text_.size() || !is_digit(text_[p])) {
                return set_error(p, "digit expected after the decimal point");
            }
            while (p < text_.size() && is_digit(text_[p])) {
                ++p;
            }
        }
        if (p < text_.size() && (text_[p] == 'e' || text_[p] == 'E')) {
            integral = false;
            ++p;
            if (p < text_.size() && (text_[p] == '+' || text_[p] == '-')) {
                ++p;
            }
            if (p >= text_.size() || !is_digit(text_[p])) {
                return set_error(p, "digit expected in the exponent");
            }
            while (p < text_.size() && is_digit(text_[p])) {
                ++p;
            }
        }
        const char* first = text_.data() + start;
        const char* last = text_.data() + p;
        pos_ = p;
        if (integral) {
            std::int64_t v = 0;
            const std::from_chars_result r = std::from_chars(first, last, v);
            if (r.ec == std::errc() && r.ptr == last) {
                *out = Value::integer(v);
                return true;
            }
            // Too large for int64: fall through to double.
        }
        double d = 0.0;
        const std::from_chars_result r = std::from_chars(first, last, d);
        if (r.ec != std::errc() || r.ptr != last || !std::isfinite(d)) {
            return set_error(start, "number out of range");
        }
        *out = Value::number(d);
        return true;
    }

    static int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool parse_hex4(std::uint32_t* out) {
        if (pos_ + 4 > text_.size()) {
            return set_error(pos_, "truncated \\u escape");
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const int h = hex_value(text_[pos_ + static_cast<std::size_t>(i)]);
            if (h < 0) {
                return set_error(pos_, "invalid hex digit in \\u escape");
            }
            v = v * 16 + static_cast<std::uint32_t>(h);
        }
        pos_ += 4;
        *out = v;
        return true;
    }

    static void append_utf8(std::string* s, std::uint32_t cp) {
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

    bool parse_string(std::string* out) {
        ++pos_;  // opening quote
        out->clear();
        for (;;) {
            if (pos_ >= text_.size()) {
                return set_error(pos_, "unterminated string");
            }
            const unsigned char c = static_cast<unsigned char>(text_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) {
                return set_error(pos_, "raw control character in string");
            }
            if (out->size() >= kMaxStringBytes) {
                return set_error(pos_, "string is longer than 256 KiB");
            }
            if (c != '\\') {
                out->push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= text_.size()) {
                return set_error(pos_, "unterminated escape");
            }
            const char e = text_[pos_++];
            switch (e) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!parse_hex4(&cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                            return set_error(pos_, "high surrogate without a low surrogate");
                        }
                        pos_ += 2;
                        std::uint32_t low = 0;
                        if (!parse_hex4(&low)) {
                            return false;
                        }
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return set_error(pos_, "high surrogate followed by a non-low-surrogate");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return set_error(pos_, "lone low surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return set_error(pos_ - 1, "invalid escape sequence");
            }
        }
    }

    bool parse_array(Value* out, std::size_t depth) {
        if (depth >= kMaxDepth) {
            return set_error(pos_, "nesting deeper than 32 levels");
        }
        ++pos_;  // [
        Value array = Value::array();
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            *out = std::move(array);
            return true;
        }
        for (;;) {
            if (array.size() >= kMaxElements) {
                return set_error(pos_, "more than 65536 array elements");
            }
            skip_space();
            Value element;
            if (!parse_value(&element, depth + 1)) {
                return false;
            }
            array.push(std::move(element));
            skip_space();
            if (pos_ >= text_.size()) {
                return set_error(pos_, "unterminated array");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                *out = std::move(array);
                return true;
            }
            return set_error(pos_, "',' or ']' expected in array");
        }
    }

    bool parse_object(Value* out, std::size_t depth) {
        if (depth >= kMaxDepth) {
            return set_error(pos_, "nesting deeper than 32 levels");
        }
        ++pos_;  // {
        Value object = Value::object();
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            *out = std::move(object);
            return true;
        }
        for (;;) {
            if (object.size() >= kMaxElements) {
                return set_error(pos_, "more than 65536 object members");
            }
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return set_error(pos_, "object key (a string) expected");
            }
            const std::size_t key_pos = pos_;
            std::string key;
            if (!parse_string(&key)) {
                return false;
            }
            if (object.find(key) != nullptr) {
                return set_error(key_pos, "duplicate object key '" + key + "'");
            }
            skip_space();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return set_error(pos_, "':' expected after object key");
            }
            ++pos_;
            skip_space();
            Value member;
            if (!parse_value(&member, depth + 1)) {
                return false;
            }
            object.set(std::move(key), std::move(member));
            skip_space();
            if (pos_ >= text_.size()) {
                return set_error(pos_, "unterminated object");
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                *out = std::move(object);
                return true;
            }
            return set_error(pos_, "',' or '}' expected in object");
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::optional<Diag> error_;
};

void write_string(std::string* out, std::string_view text) {
    std::string clean;
    if (!is_valid_utf8(text)) {
        clean = sanitize_utf8(text, text.size() * 3 + 3);
        text = clean;
    }
    out->push_back('"');
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\b': out->append("\\b"); break;
            case '\f': out->append("\\f"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out->append(buf);
                } else {
                    out->push_back(ch);
                }
        }
    }
    out->push_back('"');
}

void write_value(std::string* out, const Value& value) {
    switch (value.type()) {
        case Type::Null:
            out->append("null");
            break;
        case Type::Bool:
            out->append(value.as_bool() ? "true" : "false");
            break;
        case Type::Integer:
            out->append(std::to_string(value.as_integer()));
            break;
        case Type::Double: {
            const double d = value.as_double();
            if (!std::isfinite(d)) {
                out->append("null");
                break;
            }
            char buf[64];
            const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), d);
            std::string text(buf, r.ec == std::errc() ? r.ptr : buf);
            if (text.find_first_of(".eE") == std::string::npos) {
                text += ".0";  // keep the double-ness when read back
            }
            out->append(text);
            break;
        }
        case Type::String:
            write_string(out, value.as_string());
            break;
        case Type::Array: {
            out->push_back('[');
            bool first = true;
            for (const Value& item : value.items()) {
                if (!first) {
                    out->push_back(',');
                }
                first = false;
                write_value(out, item);
            }
            out->push_back(']');
            break;
        }
        case Type::Object: {
            out->push_back('{');
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (i != 0) {
                    out->push_back(',');
                }
                write_string(out, value.key_at(i));
                out->push_back(':');
                write_value(out, value.value_at(i));
            }
            out->push_back('}');
            break;
        }
    }
}

}  // namespace

Result<Value> parse(std::string_view text) {
    Parser parser(text);
    return parser.run();
}

std::string serialize(const Value& value) {
    std::string out;
    write_value(&out, value);
    return out;
}

}  // namespace cml::ipc::json
