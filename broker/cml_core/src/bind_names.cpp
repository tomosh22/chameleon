#include "cml/core/bind_names.h"

#include <cstddef>
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

Scope parse_qualified(Reader& r) {
    Scope sc;
    while (r.ok && !r.at_end() && r.peek() != '@') {
        if (r.peek() == '?' && is_digit(r.peek(1))) {
            // Function scope of a local entity: "?<n>?" followed by the enclosing function's own name.
            r.i += 2;
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
        if (d[k] == '?' && is_digit(d[k + 1]) && d[k + 2] == '?' && d[k + 3] == '?') {
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

bool is_guard_variable(std::string_view d) {
    if (!starts_with(d, "?$")) {
        return false;
    }
    const std::size_t e = d.find('@', 2);
    if (e == std::string_view::npos) {
        return false;
    }
    return is_guard_name(std::string(d.substr(2, e - 2)));
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
