#pragma once

// A minimal, bounds-checked reader for the TOML subset used by cmltest scenario files.
//
// Supported:
//   * comments (# to end of line), blank lines, LF or CRLF line ends, an optional UTF-8 BOM;
//   * `key = value` with bare keys ([A-Za-z0-9_-]+);
//   * values: basic strings "..." (escapes \" \\ \n \t \r \b \f \uXXXX \UXXXXXXXX), literal strings
//     '...', decimal integers (optional sign, '_' between digits), booleans true/false;
//   * tables `[name]` and arrays of tables `[[name]]` (bare names, no dots), a trailing comment
//     after the header or value is fine.
// NOT supported (reported as CML5410): dotted keys, quoted keys, inline tables, arrays of values,
// multi-line strings, floats, dates, hex/octal/binary integers, nested table paths.
//
// Limits: 1 MiB per document, 64 KiB per line, 4096 tables in total, 1024 keys per table.
// Duplicate keys, duplicate tables and mixing `[x]` with `[[x]]` are errors. Every error names the
// line.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cml/util/result.h"

namespace cmltest::toml {

struct Value {
    enum class Kind { String, Integer, Boolean };
    Kind kind = Kind::String;
    std::string text;
    std::int64_t integer = 0;
    bool boolean = false;
    int line = 0;
};

struct Table {
    std::string name;  // "" for the root table
    int line = 0;
    std::vector<std::pair<std::string, Value>> entries;  // in file order

    const Value* find(std::string_view key) const;
};

struct Document {
    Table root;
    std::vector<Table> tables;        // [name] tables in file order
    std::vector<Table> array_tables;  // [[name]] entries in file order (all names interleaved)

    const Table* find_table(std::string_view name) const;
};

cml::Result<Document> parse(std::string_view text);

inline constexpr std::size_t kMaxDocumentBytes = 1u << 20;
inline constexpr std::size_t kMaxLineBytes = 64u * 1024u;
inline constexpr std::size_t kMaxTables = 4096;
inline constexpr std::size_t kMaxKeysPerTable = 1024;

}  // namespace cmltest::toml
