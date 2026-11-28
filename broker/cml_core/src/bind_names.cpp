#include "cml/core/bind_names.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace cml::core::bind {

namespace {

bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// A reader over a decorated name. It never throws; `ok` turns false on anything unexpected.
struct Reader {
    std::string_view s;
    std::size_t i = 0;
    bool ok = true;

    bool at_end() const noexcept { return i >= s.size(); }
    char peek(std::size_t k = 0) const noexcept { return i + k < s.size() ? s[i + k] : '\0'; }
    char get() noexcept {
        if (at_end()) {
            ok = false;
            return '\0';
        }
        return s[i++];
    }
    // Text up to the next '@' (consumed).
    std::string identifier() {
        const std::size_t e = s.find('@', i);
        if (e == std::string_view::npos) {
            ok = false;
            std::string r(s.substr(i));
            i = s.size();
            return r;
        }
        std::string r(s.substr(i, e - i));
        i = e + 1;
        return r;
    }
};

struct Scope {
    std::vector<std::string> frags;  // innermost first
    std::string nested;              // display of the enclosing function when the name is function-local
};

Scope parse_qualified(Reader& r);
std::string parse_decorated(Reader& r, std::string* nested_out);

std::string join_scope(const Scope& sc, const char* last_override = nullptr) {
    std::string out;
    if (!sc.nested.empty()) {
        out = sc.nested;
    }
    for (std::size_t k = sc.frags.size(); k > 0; --k) {
        if (!out.empty()) {
            out += "::";
        }
        out += sc.frags[k - 1];
    }
    if (last_override != nullptr) {
        if (!out.empty()) {
            out += "::";
        }
        out += last_override;
    }
    return out;
}

bool is_guard_name(const std::string& name) {
    std::size_t k = 0;
    if (starts_with(name, "TSS")) {
        k = 3;
    } else if (!name.empty() && name[0] == 'S') {
        k = 1;
    } else {
        return false;
    }
    if (k >= name.size()) {
        return false;
    }
    for (; k < name.size(); ++k) {
        if (!is_digit(name[k])) {
            return false;
        }
    }
    return true;
}

std::string parse_type(Reader& r);

std::string parse_number(Reader& r) {
    bool neg = false;
    if (r.peek() == '?') {
        neg = true;
        ++r.i;
    }
    long long v = 0;
    if (is_digit(r.peek())) {
        v = r.get() - '0' + 1;
    } else {
        while (r.peek() >= 'A' && r.peek() <= 'P') {
            v = v * 16 + (r.get() - 'A');
        }
        if (r.peek() == '@') {
            ++r.i;
        } else {
            r.ok = false;
        }
    }
    return std::to_string(neg ? -v : v);
}

std::string parse_fragment(Reader& r) {
    if (r.peek() == '?' && r.peek(1) == '$') {
        r.i += 2;
        std::string name = r.identifier();
        if (!r.ok) {
            return name;
        }
        std::string args;
        bool first = true;
        while (r.ok && !r.at_end() && r.peek() != '@') {
            const std::string t = parse_type(r);
            if (!first) {
                args += ", ";
            }
            first = false;
            args += t;
        }
        if (r.peek() == '@') {
            ++r.i;
        } else {
            r.ok = false;
        }
        return name + "<" + args + ">";
    }
    if (r.peek() == '?' && r.peek(1) == 'A' && r.peek(2) == '0') {
        r.identifier();
        return "(anonymous namespace)";
    }
    std::string id = r.identifier();
    if (id.empty()) {
        r.ok = false;
    }
    return id;
}

// The length of "?<number>" at `at` when it opens the function scope of a local entity ("?<number>?" followed by the
// enclosing function's own decorated name), else 0. The number is the block the entity was declared in: one digit
// ('0'..'9' stand for 1..10), or, from block 11 on, hexadecimal letters 'A'..'P' closed by '@' (cl writes block 11 as
// "?L@?", observed in Godot's scene_tree.cpp). The letters form is only taken when the nested name follows ("@?"), so
// an anonymous namespace ("?A0x1234abcd@") and a template ("?$") are never mistaken for it.
std::size_t function_scope_length(std::string_view s, std::size_t at) noexcept {
    if (at + 1 >= s.size() || s[at] != '?') {
        return 0;
    }
    if (is_digit(s[at + 1])) {
        return 2;
    }
    std::size_t k = at + 1;
    while (k < s.size() && s[k] >= 'A' && s[k] <= 'P') {
        ++k;
    }
    if (k == at + 1 || k + 1 >= s.size() || s[k] != '@' || s[k + 1] != '?') {
        return 0;
    }
    return k + 1 - at;
}

Scope parse_qualified(Reader& r) {
    Scope sc;
    while (r.ok && !r.at_end() && r.peek() != '@') {
        if (const std::size_t scope = function_scope_length(r.s, r.i); scope != 0) {
            // Function scope of a local entity: "?<n>?" followed by the enclosing function's own name.
            r.i += scope;
            if (r.peek() == '?') {
                ++r.i;
            }
            std::string inner_nested;
            std::string inner = parse_decorated(r, &inner_nested);
            sc.nested = inner;
            return sc;
        }
        sc.frags.push_back(parse_fragment(r));
    }
    if (r.peek() == '@') {
        ++r.i;
    }
    return sc;
}

std::string parse_type(Reader& r) {
    const char c = r.get();
    switch (c) {
        case 'X': return "void";
        case 'D': return "char";
        case 'C': return "signed char";
        case 'E': return "unsigned char";
        case 'F': return "short";
        case 'G': return "unsigned short";
        case 'H': return "int";
        case 'I': return "unsigned int";
        case 'J': return "long";
        case 'K': return "unsigned long";
        case 'M': return "float";
        case 'N': return "double";
        case 'O': return "long double";
        case '_': {
            const char c2 = r.get();
            switch (c2) {
                case 'N': return "bool";
                case 'J': return "__int64";
                case 'K': return "unsigned __int64";
                case 'W': return "wchar_t";
                case 'S': return "char16_t";
                case 'U': return "char32_t";
                case 'Q': return "char8_t";
                default: r.ok = false; return "?";
            }
        }
        case 'V':
        case 'U':
        case 'T': return join_scope(parse_qualified(r));
        case 'W': {
            if (r.get() != '4') {
                r.ok = false;
            }
            return join_scope(parse_qualified(r));
        }
        case 'P':
        case 'Q':
        case 'R':
        case 'S':
        case 'A':
        case 'B': {
            if (r.peek() == 'E') {
                ++r.i;
            }
            if (r.peek() >= 'A' && r.peek() <= 'D') {
                ++r.i;
            }
            const std::string t = parse_type(r);
            return t + ((c == 'A' || c == 'B') ? "&" : "*");
        }
        case '$': {
            if (r.get() == '0') {
                return parse_number(r);
            }
            r.ok = false;
            return "?";
        }
        default: r.ok = false; return "?";
    }
}

const char* operator_name(char c) {
    switch (c) {
        case '2': return "operator new";
        case '3': return "operator delete";
        case '4': return "operator=";
        case '5': return "operator>>";
        case '6': return "operator<<";
        case '7': return "operator!";
        case '8': return "operator==";
        case '9': return "operator!=";
        case 'A': return "operator[]";
        case 'C': return "operator->";
        case 'D': return "operator*";
        case 'E': return "operator++";
        case 'F': return "operator--";
        case 'G': return "operator-";
        case 'H': return "operator+";
        case 'I': return "operator&";
        case 'J': return "operator->*";
        case 'K': return "operator/";
        case 'L': return "operator%";
        case 'M': return "operator<";
        case 'N': return "operator<=";
        case 'O': return "operator>";
        case 'P': return "operator>=";
        case 'Q': return "operator,";
        case 'R': return "operator()";
        case 'S': return "operator~";
        case 'T': return "operator^";
        case 'U': return "operator|";
        case 'V': return "operator&&";
        case 'W': return "operator||";
        case 'X': return "operator*=";
        case 'Y': return "operator+=";
        case 'Z': return "operator-=";
        default: return nullptr;
    }
}

// Parses a decorated name starting at its first '?'; returns the display of the qualified name (no
// type). `nested_out`, when given, receives the function scope of a local entity.
std::string parse_decorated(Reader& r, std::string* nested_out) {
    if (r.get() != '?') {
        r.ok = false;
        return {};
    }
    std::string display;
    if (r.peek() == '?' && r.peek(1) != '$') {
        // Special names: ??0 ctor, ??1 dtor, ??_x, ??__E, ??R ...
        ++r.i;  // second '?'
        const char c = r.get();
        if (c == '0' || c == '1') {
            Scope sc = parse_qualified(r);
            if (sc.frags.empty()) {
                r.ok = false;
                return {};
            }
            const std::string last = (c == '1' ? "~" : "") + sc.frags.front();
            display = join_scope(sc, last.c_str());
            if (nested_out != nullptr) {
                *nested_out = sc.nested;
            }
            return display;
        }
        if (c == '_') {
            const char c2 = r.get();
            if (c2 == '7' || c2 == 'G' || c2 == 'E') {
                Scope sc = parse_qualified(r);
                const char* what = c2 == '7' ? "`vftable'" : (c2 == 'G' ? "`scalar deleting destructor'" : "`vector deleting destructor'");
                return join_scope(sc, what);
            }
            if (c2 == 'C') {
                return "`string'";
            }
            if (c2 == '_') {
                const char c3 = r.get();
                if (c3 == 'E' || c3 == 'F') {
                    // The variable's own (possibly fully decorated) name follows.
                    std::string var;
                    if (r.peek() == '?') {
                        const std::size_t end = r.s.rfind("@@YAXXZ");
                        std::string_view whole = r.s.substr(r.i, end == std::string_view::npos ? std::string_view::npos : end - r.i);
                        Reader inner{whole, 0, true};
                        var = parse_decorated(inner, nullptr);
                        r.i = r.s.size();
                    } else {
                        Scope sc = parse_qualified(r);
                        var = join_scope(sc);
                    }
                    return std::string(c3 == 'E' ? "`dynamic initializer for '" : "`dynamic atexit destructor for '") + var + "''";
                }
            }
            r.ok = false;
            return {};
        }
        const char* op = operator_name(c);
        if (op == nullptr) {
            r.ok = false;
            return {};
        }
        Scope sc = parse_qualified(r);
        if (nested_out != nullptr) {
            *nested_out = sc.nested;
        }
        return join_scope(sc, op);
    }
    Scope sc = parse_qualified(r);
    if (nested_out != nullptr) {
        *nested_out = sc.nested;
    }
    return join_scope(sc);
}

}  // namespace

std::string display_name(std::string_view decorated) {
    if (decorated.empty() || decorated[0] != '?') {
        return std::string(decorated);
    }
    Reader r{decorated, 0, true};
    std::string out = parse_decorated(r, nullptr);
    if (!r.ok || out.empty()) {
        return std::string(decorated);
    }
    return out;
}

std::string base_identifier(std::string_view d) {
    if (!d.empty() && d[0] != '?') {
        // clang names internal-linkage data and functions with the plain identifier ("k_weights").
        for (const char c : d) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            if (!ok) {
                return {};
            }
        }
        return is_digit(d[0]) ? std::string() : std::string(d);
    }
    if (d.size() < 2) {
        return {};
    }
    std::string_view body = d.substr(1);
    if (starts_with(body, "?$")) {  // function template: ??$name@args@@...
        body = body.substr(2);
    } else if (body[0] == '?') {    // ctor, dtor, operator, vftable ...
        Reader r{d, 2, true};
        const char c = r.get();
        if (c == '0' || c == '1') {  // the PDB names them after the class: "Tracker", "~Tracker"
            const Scope sc = parse_qualified(r);
            if (!r.ok || sc.frags.empty()) {
                return {};
            }
            std::string n = sc.frags.front();
            const std::size_t lt = n.find('<');
            if (lt != std::string::npos) {
                n.resize(lt);
            }
            return c == '1' ? "~" + n : n;
        }
        if (const char* op = operator_name(c)) {
            return op;
        }
        return {};
    }
    const std::size_t e = body.find('@');
    if (e == std::string_view::npos || e == 0) {
        return {};
    }
    return std::string(body.substr(0, e));
}

namespace {
bool is_c_identifier(std::string_view s) noexcept {
    if (s.empty() || is_digit(s[0])) {
        return false;
    }
    for (const char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}
bool all_digits(std::string_view s) noexcept {
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        if (!is_digit(c)) {
            return false;
        }
    }
    return true;
}
}  // namespace

CStaticName c_static_name(std::string_view n) {
    CStaticName out;
    if (n.empty() || n[0] == '?') {
        return out;
    }
    if (is_c_identifier(n)) {
        out.base = std::string(n);
        return out;
    }
    // clang: "a", "a.b", with an optional trailing ".<n>" uniquifier.
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t k = 0; k <= n.size(); ++k) {
        if (k == n.size() || n[k] == '.') {
            parts.push_back(n.substr(start, k - start));
            start = k + 1;
        }
    }
    if (parts.size() >= 2 && all_digits(parts.back())) {
        parts.pop_back();
    }
    for (const std::string_view part : parts) {
        if (!is_c_identifier(part)) {
            return out;
        }
    }
    if (parts.size() == 1) {
        out.base = std::string(parts[0]);
    } else if (parts.size() == 2 && parts[0] != "__const" && parts[0] != "__func__") {
        out.encl = std::string(parts[0]);
        out.base = std::string(parts[1]);
    }
    return out;
}

bool is_compiler_generated_data_name(std::string_view n) {
    if (n.empty() || n[0] == '?' || n[0] == '.' || n[0] == '$') {
        return true;
    }
    static constexpr std::string_view kPrefixes[] = {"__const.", "__func__.", "__profc_", "__profd_", "__profn_", "__covrec_",
                                                     "__llvm_", "___asan_", "__asan_", "__odr_", "__sancov", "__real@",
                                                     "__xmm@",  "__ymm@",   "__zmm@"};
    for (const std::string_view p : kPrefixes) {
        if (starts_with(n, p)) {
            return true;
        }
    }
    return false;
}

bool is_split_piece_name(std::string_view n) {
    const std::size_t dot = n.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= n.size() || !all_digits(n.substr(dot + 1))) {
        return false;
    }
    const std::string_view head = n.substr(0, dot);
    if (head[0] == '?') {
        return head.size() > 1 && head.find('.') == std::string_view::npos;
    }
    return !is_compiler_generated_data_name(n) && !c_static_name(n).base.empty();
}

std::string_view call_operator_scope(std::string_view d) {
    // '?' <identifier> '@' '?' <number> '?' <nested decorated name>, the nested name being "??R<class>...".
    if (d.size() < 2 || d[0] != '?') {
        return {};
    }
    std::size_t at = d.find('@', 1);
    if (at == std::string_view::npos) {
        return {};
    }
    ++at;
    if (at >= d.size() || d[at] != '?') {
        return {};
    }
    // The scope number (function_scope_length), then the '?' that opens the nested name.
    const std::size_t scope = function_scope_length(d, at);
    if (scope == 0) {
        return {};
    }
    at += scope;
    if (at >= d.size() || d[at] != '?') {
        return {};
    }
    ++at;
    if (d.substr(at, 3) != "??R") {
        return {};
    }
    return d.substr(at + 3);
}

std::string enclosing_function(std::string_view decorated) {
    if (decorated.empty() || decorated[0] != '?') {
        return {};
    }
    Reader r{decorated, 0, true};
    std::string nested;
    (void)parse_decorated(r, &nested);
    if (!r.ok) {
        return {};
    }
    return nested.empty() ? std::string() : last_component(nested);
}

bool is_tu_private_name(std::string_view d) {
    if (d.empty() || d[0] != '?') {
        return false;
    }
    if (d.find("?A0x") != std::string_view::npos) {
        return true;
    }
    for (std::size_t k = 0; k + 3 < d.size(); ++k) {
        const std::size_t scope = function_scope_length(d, k);
        if (scope != 0 && k + scope + 1 < d.size() && d[k + scope] == '?' && d[k + scope + 1] == '?') {
            return true;
        }
    }
    return false;
}

std::string last_component(std::string_view n) {
    // Find the last "::" outside of <...>, (...), `...'.
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t k = 0; k < n.size(); ++k) {
        const char c = n[k];
        if (c == '<' || c == '(' || c == '`') {
            ++depth;
        } else if ((c == '>' || c == ')' || c == '\'') && depth > 0) {
            --depth;
        } else if (c == ':' && depth == 0 && k + 1 < n.size() && n[k + 1] == ':') {
            start = k + 2;
            ++k;
        }
    }
    std::string_view tail = n.substr(start);
    // Strip trailing template arguments.
    if (!tail.empty() && tail.back() == '>') {
        int d = 0;
        for (std::size_t k = tail.size(); k > 0; --k) {
            const char c = tail[k - 1];
            if (c == '>') {
                ++d;
            } else if (c == '<') {
                --d;
                if (d == 0) {
                    tail = tail.substr(0, k - 1);
                    break;
                }
            }
        }
    }
    return std::string(tail);
}

bool is_dynamic_initializer(std::string_view d) {
    return starts_with(d, "??__E") || starts_with(d, "__cxx_global_var_init");
}

bool is_tu_wide_initializer(std::string_view d) { return starts_with(d, "_GLOBAL__sub_I_"); }

bool is_atexit_destructor(std::string_view d) { return starts_with(d, "??__F"); }

bool is_funclet(std::string_view d) {
    return starts_with(d, "?dtor$") || starts_with(d, "?catch$") || starts_with(d, "?filt$") ||
           starts_with(d, "?fin$") || starts_with(d, "?try$");
}

namespace {

// WP-59 (R-67): where the "@5<n>" that closes clang's `??_B<scope>@5<n>` starts (the index of its '@'), npos when `d` is
// not such a name. <scope> is a function scope ("?<n>?" or "?<hex>@?" and the function's decorated name), <n> a number as
// the mangling writes one (a digit, or hexadecimal letters 'A'..'P' closed by '@').
std::size_t inline_bitmask_scope_end(std::string_view d) noexcept {
    if (d.size() < 10 || !starts_with(d, "??_B?")) {
        return std::string_view::npos;
    }
    std::size_t n = d.size();
    if (is_digit(d[n - 1])) {
        n -= 1;
    } else if (d[n - 1] == '@') {
        std::size_t k = n - 1;
        while (k > 0 && d[k - 1] >= 'A' && d[k - 1] <= 'P') {
            --k;
        }
        if (k == n - 1) {
            return std::string_view::npos;
        }
        n = k;
    } else {
        return std::string_view::npos;
    }
    if (n < 6 || d[n - 1] != '5' || d[n - 2] != '@' || function_scope_length(d, 4) == 0) {
        return std::string_view::npos;
    }
    return n - 2;
}

}  // namespace

bool is_inline_bitmask_guard(std::string_view d) { return inline_bitmask_scope_end(d) != std::string_view::npos; }

std::string_view inline_bitmask_scope(std::string_view d) {
    const std::size_t end = inline_bitmask_scope_end(d);
    return end == std::string_view::npos ? std::string_view() : d.substr(4, end - 4);
}

bool is_guard_variable(std::string_view d) {
    if (is_inline_bitmask_guard(d)) {
        return true;
    }
    if (!starts_with(d, "?$")) {
        return false;
    }
    const std::size_t e = d.find('@', 2);
    if (e == std::string_view::npos) {
        return false;
    }
    return is_guard_name(std::string(d.substr(2, e - 2)));
}

// ---- WP-52 (R-60): name back-references ----------------------------------------------------------------------------

namespace {

// Writes out the part of a decorated name it reads, with every name back-reference replaced by the name it stands for.
// It reads the qualified names, function encodings and types that the scopes of function-local statics contain (free and
// member functions, templates, lambdas, anonymous namespaces, pointers, references, arrays, function pointers) and
// fails on anything else (`ok` false): the caller then has no key, never a wrong one.
struct BackrefReader {
    std::string_view s;
    std::size_t i = 0;
    bool ok = true;
    int depth = 0;
    std::vector<std::string> names;  // the back-reference table: each name with its closing '@'

    char peek(std::size_t k = 0) const noexcept { return i + k < s.size() ? s[i + k] : '\0'; }
    bool fail() noexcept {
        ok = false;
        return false;
    }
    bool take(char c, std::string& out) {
        if (!ok || peek() != c) {
            return fail();
        }
        out += c;
        ++i;
        return true;
    }
    bool take_any(std::string& out) {
        if (!ok || i >= s.size()) {
            return fail();
        }
        out += s[i++];
        return true;
    }
    static bool in(char c, std::string_view set) noexcept { return c != '\0' && set.find(c) != std::string_view::npos; }
    void memorize(const std::string& name) {
        if (names.size() < 10 && std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    }
    // A simple name up to and with its '@'.
    bool identifier(std::string& name) {
        const std::size_t e = s.find('@', i);
        if (!ok || e == std::string_view::npos || e == i) {
            return fail();
        }
        name.assign(s.substr(i, e + 1 - i));
        i = e + 1;
        return true;
    }
    bool backref(std::string& out) {
        const std::size_t k = static_cast<std::size_t>(peek() - '0');
        if (k >= names.size()) {
            return fail();
        }
        out += names[k];
        ++i;
        return true;
    }
    // "<digit>" (1..10), or hexadecimal letters 'A'..'P' closed by '@'; an optional leading '?' (negative).
    bool number(std::string& out, std::uint64_t* value = nullptr) {
        if (peek() == '?') {
            take_any(out);
        }
        std::uint64_t v = 0;
        if (is_digit(peek())) {
            v = static_cast<std::uint64_t>(peek() - '0') + 1;
            take_any(out);
        } else {
            while (peek() >= 'A' && peek() <= 'P') {
                v = v * 16 + static_cast<std::uint64_t>(peek() - 'A');
                take_any(out);
            }
            take('@', out);
        }
        if (value != nullptr) {
            *value = v;
        }
        return ok;
    }
    // Scope pieces up to the '@' that closes the qualified name.
    bool chain(std::string& out) {
        while (ok && peek() != '@') {
            if (i >= s.size() || !piece(out)) {
                return fail();
            }
        }
        return take('@', out);
    }
    bool piece(std::string& out) {
        const char c = peek();
        if (is_digit(c)) {
            return backref(out);
        }
        if (c == '?') {
            if (peek(1) == '$') {
                std::string t;
                if (!template_name(t)) {
                    return false;
                }
                memorize(t);
                out += t;
                return true;
            }
            if (const std::size_t len = function_scope_length(s, i); len != 0) {
                out += s.substr(i, len);
                i += len;
                return take('?', out) && local_symbol(out);
            }
            if (peek(1) == 'A') {  // anonymous namespace "?A0x1234abcd@"
                std::string t;
                if (!identifier(t)) {
                    return false;
                }
                memorize(t);
                out += t;
                return true;
            }
            return fail();
        }
        std::string t;
        if (!identifier(t)) {
            return false;
        }
        memorize(t);
        out += t;
        return true;
    }
    // "?$name@<arguments>@": the arguments are read with a table of their own, which starts with the template's name; the
    // whole template is ONE entry of the enclosing table (memorised by the caller, except for a symbol's own name).
    bool template_name(std::string& out) {
        if (++depth > 64) {
            return fail();
        }
        out += "?$";
        i += 2;
        std::vector<std::string> outer;
        outer.swap(names);
        std::string name;
        if (peek() == '?' || !identifier(name)) {
            names.swap(outer);
            return fail();  // an operator template ("??$?6...") is not read
        }
        memorize(name);
        out += name;
        while (ok && peek() != '@') {
            if (i >= s.size() || !type(out)) {
                fail();
                break;
            }
        }
        take('@', out);
        names.swap(outer);
        --depth;
        return ok;
    }
    // A function scope: the enclosing function's own decorated name.
    bool local_symbol(std::string& out) { return symbol(out); }
    // '?' <name> <scope pieces> '@' <encoding>
    bool symbol(std::string& out) {
        if (++depth > 64 || !take('?', out)) {
            return fail();
        }
        if (peek() == '?') {
            if (peek(1) == '$') {
                if (!template_name(out)) {  // a function template's own name is not memorised
                    return false;
                }
            } else {
                take_any(out);  // the '?' of an operator, constructor or destructor
                if (peek() == '_') {
                    if (peek(1) == '_' || peek(1) == '\0') {
                        return fail();
                    }
                    take_any(out);
                }
                if (!(is_digit(peek()) || (peek() >= 'A' && peek() <= 'Z'))) {
                    return fail();
                }
                take_any(out);
            }
        } else {
            std::string t;
            if (!identifier(t)) {
                return false;
            }
            memorize(t);
            out += t;
        }
        if (!chain(out) || !encoding(out)) {
            return false;
        }
        --depth;
        return ok;
    }
    bool encoding(std::string& out) {
        const char c = peek();
        if (c == 'Y' || c == 'Z' || in(c, "CDKLST")) {  // free function, static member function
            take_any(out);
            return function_tail(out);
        }
        if (in(c, "ABEFIJMNQRUV")) {  // member function: the qualifiers of `this`, then the function
            take_any(out);
            while (in(peek(), "EFI")) {
                take_any(out);
            }
            if (in(peek(), "GH")) {
                take_any(out);
            }
            if (!in(peek(), "ABCD")) {
                return fail();
            }
            take_any(out);
            return function_tail(out);
        }
        if (c == '9') {  // a C function (cl decorates the statics of C functions: "?counter@?1??next@@9@9")
            return take_any(out);
        }
        if (c >= '0' && c <= '4') {  // a variable (a template argument `$1?x@@3HA`)
            take_any(out);
            if (!type(out)) {
                return false;
            }
            while (in(peek(), "EFI")) {
                take_any(out);
            }
            if (!in(peek(), "ABCD")) {
                return fail();
            }
            return take_any(out);
        }
        return fail();
    }
    // <calling convention> <return type> <parameters> <throw specification>
    bool function_tail(std::string& out) {
        if (!(peek() >= 'A' && peek() <= 'Z')) {
            return fail();
        }
        take_any(out);
        if (!return_type(out) || !parameters(out)) {
            return false;
        }
        if (peek() == 'Z') {
            return take_any(out);
        }
        if (peek() == '_' && peek(1) == 'E') {
            take_any(out);
            return take_any(out);
        }
        return fail();
    }
    bool return_type(std::string& out) {
        if (peek() == '@') {  // constructor, destructor
            return take_any(out);
        }
        if (peek() == '?') {  // qualified return type, or a deduced one: "?A?<auto>@@"
            take_any(out);
            if (!in(peek(), "ABCD")) {
                return fail();
            }
            take_any(out);
            if (peek() == '?') {
                take_any(out);
                if (is_digit(peek())) {
                    if (!backref(out)) {
                        return false;
                    }
                } else {
                    std::string t;
                    if (!identifier(t)) {
                        return false;
                    }
                    memorize(t);
                    out += t;
                }
                return take('@', out);
            }
        }
        return type(out);
    }
    bool parameters(std::string& out) {
        if (peek() == 'X') {
            return take_any(out);
        }
        while (ok) {
            const char c = peek();
            if (c == '@' || c == 'Z') {  // end of the list, or "..." (then the throw specification)
                return take_any(out);
            }
            if (c == '\0') {
                return fail();
            }
            if (is_digit(c)) {  // a type back-reference: the parameter lists in front of it are the same in both names
                take_any(out);
                continue;
            }
            if (!type(out)) {
                return false;
            }
        }
        return false;
    }
    bool type(std::string& out) {
        if (++depth > 64) {
            return fail();
        }
        const char c = peek();
        bool read = false;
        if (in(c, "CDEFGHIJKMNOX")) {
            read = take_any(out);
        } else if (c == '_') {
            take_any(out);
            read = peek() >= 'A' && peek() <= 'Z' ? take_any(out) : fail();
        } else if (in(c, "TUV")) {
            take_any(out);
            read = chain(out);
        } else if (c == 'W') {
            take_any(out);
            read = is_digit(peek()) ? take_any(out) && chain(out) : fail();
        } else if (in(c, "PQRSAB")) {
            take_any(out);
            read = pointee(out);
        } else if (c == 'Y') {
            take_any(out);
            std::uint64_t dims = 0;
            read = number(out, &dims) && dims <= 16;
            for (std::uint64_t k = 0; read && k < dims; ++k) {
                read = number(out);
            }
            read = read && type(out);
        } else if (c == '?') {
            take_any(out);
            read = in(peek(), "ABCD") ? take_any(out) && type(out) : fail();
        } else if (c == '$') {
            read = dollar(out);
        } else {
            read = fail();
        }
        --depth;
        return read && ok;
    }
    bool pointee(std::string& out) {
        while (in(peek(), "EFI")) {
            take_any(out);
        }
        if (peek() == '6') {
            take_any(out);
            return function_tail(out);
        }
        if (in(peek(), "ABCD")) {
            take_any(out);
            return type(out);
        }
        return fail();
    }
    // The '$' forms of types and template arguments.
    bool dollar(std::string& out) {
        if (peek(1) == '$') {
            const char c = peek(2);
            out += s.substr(i, 3);
            i += 3;
            switch (c) {
                case 'Q':
                case 'R': return pointee(out);  // rvalue reference
                case 'C': return in(peek(), "ABCD") && take_any(out) && type(out);
                case 'A': return peek() == '6' && take_any(out) && function_tail(out);
                case 'B': return type(out);
                case 'T':
                case 'V':
                case 'Z': return true;  // nullptr_t, an empty pack, a pack separator
                default: return fail();
            }
        }
        if (peek(1) == '0') {
            out += "$0";
            i += 2;
            return number(out);
        }
        if (peek(1) == '1' && peek(2) == '?') {
            out += "$1";
            i += 2;
            return symbol(out);
        }
        return fail();
    }
};

}  // namespace

std::optional<LocalScopeKey> local_scope_key(std::string_view d, bool guard_name_counted) {
    if (d.size() < 4 || d[0] != '?') {
        return std::nullopt;
    }
    BackrefReader r;
    r.s = d;
    r.i = 1;
    const bool inline_bitmask = is_inline_bitmask_guard(d);
    if (inline_bitmask) {
        // WP-59 (R-67): `??_B` is a special name, which the mangling never puts in the back-reference table: both readings
        // are the same, and the scope starts right after it.
        r.i = 4;
    } else if (is_guard_variable(d)) {
        const std::size_t e = d.find('@', 2);
        if (guard_name_counted) {
            r.names.emplace_back(d.substr(2, e - 1));
        }
        r.i = e + 1;
    } else {
        std::string name;
        if (d[1] == '?' || !r.identifier(name)) {
            return std::nullopt;  // an operator, a template or a special name: no static of a function
        }
        r.memorize(name);
    }
    const std::size_t len = function_scope_length(d, r.i);
    if (len == 0) {
        return std::nullopt;
    }
    LocalScopeKey key;
    key.block = std::string(d.substr(r.i, len));
    r.i += len;
    if (!r.take('?', key.block) || !r.local_symbol(key.rest)) {
        return std::nullopt;
    }
    while (r.ok && r.peek() != '@') {
        if (r.i >= d.size() || !r.piece(key.rest)) {
            return std::nullopt;
        }
    }
    if (inline_bitmask) {
        // "@5<n>" closes the bitmask's scope where "@4<type>" closes a static's: written as the statics' "@4", so that the
        // bitmask's key is the key of its function's statics.
        if (r.i != inline_bitmask_scope_end(d) || !r.take('@', key.rest) || !r.ok) {
            return std::nullopt;
        }
        key.rest += '4';
        return key;
    }
    if (!r.take('@', key.rest) || !is_digit(r.peek()) || !r.take_any(key.rest) || !r.ok) {
        return std::nullopt;
    }
    return key;
}

std::optional<std::string> resolved_qualified_name(std::string_view qualified) {
    if (qualified.empty() || qualified[0] == '@') {
        return std::nullopt;
    }
    BackrefReader r;
    r.s = qualified;
    std::string out;
    if (!r.chain(out) || !r.ok) {
        return std::nullopt;
    }
    out.append(qualified.substr(r.i));
    return out;
}

std::optional<std::string> resolved_decorated_name(std::string_view decorated) {
    if (decorated.size() < 2 || decorated[0] != '?') {
        return std::nullopt;
    }
    BackrefReader r;
    r.s = decorated;
    std::string out;
    if (!r.symbol(out) || !r.ok) {
        return std::nullopt;
    }
    out.append(decorated.substr(r.i));
    return out;
}

std::string initializer_target_prefix(std::string_view d) {
    if (!starts_with(d, "??__E")) {
        return {};
    }
    std::string_view rest = d.substr(5);
    if (rest.empty()) {
        return {};
    }
    if (rest[0] == '?') {
        const std::size_t e = rest.rfind("@@YAXXZ");
        return std::string(e == std::string_view::npos ? rest : rest.substr(0, e));
    }
    const std::size_t e = rest.find("@@");
    return std::string(e == std::string_view::npos ? rest : rest.substr(0, e + 2));
}

bool initializer_matches_global(std::string_view initializer, std::string_view global) {
    const std::string prefix = initializer_target_prefix(initializer);
    if (prefix.empty() || global.size() < 2) {
        return false;
    }
    if (prefix[0] != '?') {
        return global.substr(1, prefix.size()) == prefix;
    }
    if (global == prefix) {
        return true;
    }
    // Variable template: the initialiser embeds "?$v@H" (the template-id), the variable is "??$v@H@@3HA".
    if (starts_with(prefix, "?$")) {
        const std::string wanted = "?" + prefix + "@@";
        return starts_with(global, wanted);
    }
    return false;
}

bool is_fresh_data_name(std::string_view d) {
    return starts_with(d, "$") || starts_with(d, "??_7") || starts_with(d, "??_R") || starts_with(d, "??_C@") ||
           starts_with(d, "??_S") || starts_with(d, "??_8") || starts_with(d, "__real@") ||
           starts_with(d, "__xmm@") || starts_with(d, "__ymm@") || starts_with(d, "__zmm@");
}

}  // namespace cml::core::bind
