#pragma once

// A small JSON value, reader and writer for the control protocol (docs/wp/WP-13.md, "Control
// protocol"). Portable, no exceptions, no third-party code.
//
// The reader is strict (RFC 8259) and bounded: nesting depth, total size, string length and the
// number of elements per container are limited, so a hostile or broken peer can neither exhaust
// the stack nor the memory. Anything malformed gives CML5400 with the byte offset.
//
// Deliberate restrictions beyond RFC 8259: input must be valid UTF-8, a lone surrogate escape is an
// error, duplicate object keys are an error (ambiguity), and numbers are either int64 integers or
// finite doubles.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cml/util/result.h"

namespace cml::ipc::json {

inline constexpr std::size_t kMaxDocumentBytes = 1u << 20;   // 1 MiB
inline constexpr std::size_t kMaxDepth = 32;
inline constexpr std::size_t kMaxStringBytes = 256u * 1024u;
inline constexpr std::size_t kMaxElements = 65536;           // per array / object

enum class Type { Null, Bool, Integer, Double, String, Array, Object };

class Value {
public:
    Value() = default;  // null

    static Value boolean(bool v);
    static Value integer(std::int64_t v);
    static Value number(double v);
    static Value string(std::string v);
    static Value array();
    static Value object();

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool is_bool() const noexcept { return type_ == Type::Bool; }
    bool is_integer() const noexcept { return type_ == Type::Integer; }
    bool is_number() const noexcept { return type_ == Type::Integer || type_ == Type::Double; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_array() const noexcept { return type_ == Type::Array; }
    bool is_object() const noexcept { return type_ == Type::Object; }

    // Accessors return the fallback when the value has another type.
    bool as_bool(bool fallback = false) const noexcept;
    std::int64_t as_integer(std::int64_t fallback = 0) const noexcept;
    double as_double(double fallback = 0.0) const noexcept;
    const std::string& as_string() const noexcept;  // empty string for non-strings

    // Arrays.
    const std::vector<Value>& items() const noexcept { return values_; }
    Value& push(Value v);  // no effect unless this is an array; returns the stored element (or *this)

    // Objects (insertion order is kept; set() replaces an existing key).
    std::size_t size() const noexcept { return values_.size(); }
    const std::string& key_at(std::size_t i) const noexcept { return keys_[i]; }
    const Value& value_at(std::size_t i) const noexcept { return values_[i]; }
    const Value* find(std::string_view key) const noexcept;  // nullptr when absent or not an object
    Value& set(std::string key, Value v);                    // no effect unless this is an object

    // Typed lookups on objects; the fallback is returned when the key is missing or mistyped.
    std::string get_string(std::string_view key, const std::string& fallback = std::string()) const;
    std::int64_t get_integer(std::string_view key, std::int64_t fallback = 0) const noexcept;
    bool get_bool(std::string_view key, bool fallback = false) const noexcept;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    std::int64_t int_ = 0;
    double double_ = 0.0;
    std::string string_;
    std::vector<std::string> keys_;  // objects only, parallel to values_
    std::vector<Value> values_;      // array elements or object members
};

// Parses exactly one JSON value; surrounding whitespace is allowed, anything else after it is not.
Result<Value> parse(std::string_view text);

// Compact serialisation on a single line (no raw control characters, so it is safe as one line of
// the control protocol). Non-finite doubles are written as null. Strings that are not valid UTF-8
// are written with U+FFFD for the bad bytes.
std::string serialize(const Value& value);

}  // namespace cml::ipc::json
