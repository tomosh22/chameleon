#include "rx.h"

#include <cstring>
#include <memory>
#include <utility>

#include "cml/ipc/control_ids.h"

namespace cmltest {

namespace {

constexpr int kInfinite = -1;
constexpr int kMaxRepeat = 1000;
constexpr std::size_t kMaxStack = 1u << 20;

bool is_word_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

}  // namespace

// Pattern -> AST -> program.
class RegexCompiler {
public:
    struct Node {
        enum class Type { Empty, Char, Any, Class, Bol, Eol, WordB, NotWordB, Cat, Alt, Repeat };
        Type type = Type::Empty;
        char ch = 0;
        std::uint32_t class_index = 0;
        std::vector<std::uint32_t> children;
        int min = 0;
        int max = 0;
        bool lazy = false;
    };

    RegexCompiler(std::string_view pattern, Regex* out) : pattern_(pattern), out_(out) {}

    cml::Result<void> run() {
        cml::Result<std::uint32_t> root = parse_alt(0);
        if (!root) {
            return root.error();
        }
        if (pos_ < pattern_.size()) {
            return error(pos_, "unmatched ')'");
        }
        cml::Result<void> emitted = emit(*root);
        if (!emitted) {
            return emitted;
        }
        Regex::Inst match;
        match.op = Regex::Inst::Op::Match;
        return push(match);
    }

private:
    cml::Diag error(std::size_t at, const std::string& what) const {
        return cml::Diag::make(cml::ids::kRegexInvalid,
                               "invalid pattern '" + std::string(pattern_) + "' at position " + std::to_string(at) + ": " + what,
                               "Fix the pattern; the supported syntax is listed in tests/harness/README.md.");
    }

    std::uint32_t add_node(Node n) {
        nodes_.push_back(std::move(n));
        return static_cast<std::uint32_t>(nodes_.size() - 1);
    }

    bool at_end() const { return pos_ >= pattern_.size(); }

    cml::Result<std::uint32_t> parse_alt(std::size_t depth) {
        if (depth > 64) {
            return error(pos_, "groups nested deeper than 64 levels");
        }
        std::vector<std::uint32_t> branches;
        for (;;) {
            cml::Result<std::uint32_t> branch = parse_cat(depth);
            if (!branch) {
                return branch;
            }
            branches.push_back(*branch);
            if (!at_end() && pattern_[pos_] == '|') {
                ++pos_;
                continue;
            }
            break;
        }
        if (branches.size() == 1) {
            return branches[0];
        }
        Node alt;
        alt.type = Node::Type::Alt;
        alt.children = std::move(branches);
        return add_node(std::move(alt));
    }

    cml::Result<std::uint32_t> parse_cat(std::size_t depth) {
        Node cat;
        cat.type = Node::Type::Cat;
        while (!at_end() && pattern_[pos_] != '|' && pattern_[pos_] != ')') {
            cml::Result<std::uint32_t> atom = parse_atom(depth);
            if (!atom) {
                return atom;
            }
            cml::Result<std::uint32_t> quantified = parse_quantifier(*atom);
            if (!quantified) {
                return quantified;
            }
            cat.children.push_back(*quantified);
        }
        if (cat.children.empty()) {
            return add_node(Node());  // Empty
        }
        if (cat.children.size() == 1) {
            return cat.children[0];
        }
        return add_node(std::move(cat));
    }

    // Tries to read `{n}`, `{n,}` or `{n,m}` at pos_; on success consumes it.
    bool try_braces(int* min, int* max) {
        std::size_t p = pos_ + 1;
        auto number = [&](int* out) {
            const std::size_t start = p;
            long value = 0;
            while (p < pattern_.size() && pattern_[p] >= '0' && pattern_[p] <= '9' && value <= 100000) {
                value = value * 10 + (pattern_[p] - '0');
                ++p;
            }
            *out = static_cast<int>(value);
            return p > start;
        };
        int lo = 0;
        if (!number(&lo)) {
            return false;
        }
        int hi = lo;
        if (p < pattern_.size() && pattern_[p] == ',') {
            ++p;
            if (p < pattern_.size() && pattern_[p] == '}') {
                hi = kInfinite;
            } else if (!number(&hi)) {
                return false;
            }
        }
        if (p >= pattern_.size() || pattern_[p] != '}') {
            return false;
        }
        *min = lo;
        *max = hi;
        pos_ = p + 1;
        return true;
    }

    cml::Result<std::uint32_t> parse_quantifier(std::uint32_t atom) {
        if (at_end()) {
            return atom;
        }
        int min = 0;
        int max = 0;
        const char c = pattern_[pos_];
        const std::size_t at = pos_;
        if (c == '*') {
            ++pos_;
            min = 0;
            max = kInfinite;
        } else if (c == '+') {
            ++pos_;
            min = 1;
            max = kInfinite;
        } else if (c == '?') {
            ++pos_;
            min = 0;
            max = 1;
        } else if (c == '{') {
            if (!try_braces(&min, &max)) {
                return atom;  // a literal '{' follows; the next atom picks it up
            }
            if (min > kMaxRepeat || max > kMaxRepeat || (max != kInfinite && max < min)) {
                return error(at, "repeat counts must be at most 1000 and min <= max");
            }
        } else {
            return atom;
        }
        Node rep;
        rep.type = Node::Type::Repeat;
        rep.children.push_back(atom);
        rep.min = min;
        rep.max = max;
        if (!at_end() && pattern_[pos_] == '?') {
            rep.lazy = true;
            ++pos_;
        }
        if (nodes_[atom].type == Node::Type::Bol || nodes_[atom].type == Node::Type::Eol ||
            nodes_[atom].type == Node::Type::WordB || nodes_[atom].type == Node::Type::NotWordB) {
            return error(at, "nothing to repeat (assertions cannot be quantified)");
        }
        return add_node(std::move(rep));
    }

    struct Bits {
        bool v[256];
        Bits() { std::memset(v, 0, sizeof(v)); }
    };

    static void add_range(Bits* b, int lo, int hi) {
        for (int c = lo; c <= hi; ++c) {
            b->v[c] = true;
        }
    }

    // \d \w \s and their negations into `b`. Returns false when `e` is not a class escape.
    static bool class_escape(Bits* b, char e) {
        Bits positive;
        bool negate = false;
        switch (e) {
            case 'd': add_range(&positive, '0', '9'); break;
            case 'D': add_range(&positive, '0', '9'); negate = true; break;
            case 'w': for (int c = 0; c < 256; ++c) positive.v[c] = is_word_char(static_cast<unsigned char>(c)); break;
            case 'W': for (int c = 0; c < 256; ++c) positive.v[c] = is_word_char(static_cast<unsigned char>(c)); negate = true; break;
            case 's':
            case 'S':
                for (const char w : {' ', '\t', '\n', '\r', '\f', '\v'}) positive.v[static_cast<unsigned char>(w)] = true;
                negate = e == 'S';
                break;
            default:
                return false;
        }
        for (int c = 0; c < 256; ++c) {
            if (positive.v[c] != negate) {
                b->v[c] = true;
            }
        }
        return true;
    }

    static bool simple_escape(char e, char* out) {
        switch (e) {
            case 't': *out = '\t'; return true;
            case 'n': *out = '\n'; return true;
            case 'r': *out = '\r'; return true;
            case 'f': *out = '\f'; return true;
            case 'v': *out = '\v'; return true;
            case '0': *out = '\0'; return true;
            default: return false;
        }
    }

    cml::Result<std::uint32_t> parse_class() {
        const std::size_t open = pos_;
        ++pos_;  // [
        bool negate = false;
        if (!at_end() && pattern_[pos_] == '^') {
            negate = true;
            ++pos_;
        }
        Bits bits;
        bool any = false;
        for (;;) {
            if (at_end()) {
                return error(open, "unterminated character class");
            }
            char c = pattern_[pos_];
            if (c == ']') {
                ++pos_;
                break;
            }
            ++pos_;
            if (c == '\\') {
                if (at_end()) {
                    return error(pos_, "dangling backslash");
                }
                const char e = pattern_[pos_++];
                if (class_escape(&bits, e)) {
                    any = true;
                    continue;
                }
                char mapped = 0;
                c = simple_escape(e, &mapped) ? mapped : e;
            }
            // Range?
            if (pos_ + 1 < pattern_.size() && pattern_[pos_] == '-' && pattern_[pos_ + 1] != ']') {
                char hi = pattern_[pos_ + 1];
                std::size_t next = pos_ + 2;
                if (hi == '\\') {
                    if (next >= pattern_.size()) {
                        return error(next, "dangling backslash");
                    }
                    const char e = pattern_[next++];
                    if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
                        return error(pos_, "a class escape cannot end a range");
                    }
                    char mapped = 0;
                    hi = simple_escape(e, &mapped) ? mapped : e;
                }
                if (static_cast<unsigned char>(hi) < static_cast<unsigned char>(c)) {
                    return error(pos_, "range out of order");
                }
                add_range(&bits, static_cast<unsigned char>(c), static_cast<unsigned char>(hi));
                pos_ = next;
                any = true;
                continue;
            }
            bits.v[static_cast<unsigned char>(c)] = true;
            any = true;
        }
        if (!any) {
            return error(open, "empty character class");
        }
        Regex::ClassSet set = {};
        for (int c = 0; c < 256; ++c) {
            set.bits[c] = bits.v[c] != negate;
        }
        out_->classes_.push_back(set);
        Node n;
        n.type = Node::Type::Class;
        n.class_index = static_cast<std::uint32_t>(out_->classes_.size() - 1);
        return add_node(std::move(n));
    }

    cml::Result<std::uint32_t> parse_atom(std::size_t depth) {
        const char c = pattern_[pos_];
        const std::size_t at = pos_;
        Node n;
        switch (c) {
            case '(': {
                ++pos_;
                if (pattern_.substr(pos_, 2) == "?:") {
                    pos_ += 2;
                } else if (!at_end() && pattern_[pos_] == '?') {
                    return error(pos_, "only (?: ... ) groups are supported");
                }
                cml::Result<std::uint32_t> inner = parse_alt(depth + 1);
                if (!inner) {
                    return inner;
                }
                if (at_end() || pattern_[pos_] != ')') {
                    return error(at, "unterminated group");
                }
                ++pos_;
                return *inner;
            }
            case '[':
                return parse_class();
            case '.':
                ++pos_;
                n.type = Node::Type::Any;
                return add_node(std::move(n));
            case '^':
                ++pos_;
                n.type = Node::Type::Bol;
                return add_node(std::move(n));
            case '$':
                ++pos_;
                n.type = Node::Type::Eol;
                return add_node(std::move(n));
            case '*':
            case '+':
            case '?':
                return error(at, std::string("nothing to repeat before '") + c + "'");
            case '\\': {
                ++pos_;
                if (at_end()) {
                    return error(at, "dangling backslash");
                }
                const char e = pattern_[pos_++];
                if (e == 'b' || e == 'B') {
                    n.type = e == 'b' ? Node::Type::WordB : Node::Type::NotWordB;
                    return add_node(std::move(n));
                }
                Bits bits;
                if (class_escape(&bits, e)) {
                    Regex::ClassSet set = {};
                    std::memcpy(set.bits, bits.v, sizeof(set.bits));
                    out_->classes_.push_back(set);
                    n.type = Node::Type::Class;
                    n.class_index = static_cast<std::uint32_t>(out_->classes_.size() - 1);
                    return add_node(std::move(n));
                }
                if ((e >= '1' && e <= '9')) {
                    return error(at, "back-references are not supported");
                }
                char mapped = 0;
                n.type = Node::Type::Char;
                n.ch = simple_escape(e, &mapped) ? mapped : e;
                return add_node(std::move(n));
            }
            default:
                ++pos_;
                n.type = Node::Type::Char;
                n.ch = c;
                return add_node(std::move(n));
        }
    }

    // --- emission ---------------------------------------------------------------------------

    cml::Result<void> push(const Regex::Inst& inst) {
        if (out_->program_.size() >= Regex::kMaxProgram) {
            return cml::Diag::make(cml::ids::kRegexInvalid,
                                   "pattern '" + std::string(pattern_) + "' compiles to more than " +
                                       std::to_string(Regex::kMaxProgram) + " instructions",
                                   "Simplify the pattern or use smaller repeat counts.");
        }
        out_->program_.push_back(inst);
        return cml::Result<void>();
    }

    std::uint32_t here() const { return static_cast<std::uint32_t>(out_->program_.size()); }

    cml::Result<void> emit(std::uint32_t index) {
        // Copy what we need: nodes_ is not modified while emitting, so a reference stays valid.
        const Node& node = nodes_[index];
        using Op = Regex::Inst::Op;
        Regex::Inst inst;
        switch (node.type) {
            case Node::Type::Empty:
                return cml::Result<void>();
            case Node::Type::Char:
                inst.op = Op::Char;
                inst.ch = node.ch;
                return push(inst);
            case Node::Type::Any:
                inst.op = Op::Any;
                return push(inst);
            case Node::Type::Class:
                inst.op = Op::Class;
                inst.x = node.class_index;
                return push(inst);
            case Node::Type::Bol:
                inst.op = Op::Bol;
                return push(inst);
            case Node::Type::Eol:
                inst.op = Op::Eol;
                return push(inst);
            case Node::Type::WordB:
                inst.op = Op::WordB;
                return push(inst);
            case Node::Type::NotWordB:
                inst.op = Op::NotWordB;
                return push(inst);
            case Node::Type::Cat:
                for (const std::uint32_t child : node.children) {
                    cml::Result<void> r = emit(child);
                    if (!r) {
                        return r;
                    }
                }
                return cml::Result<void>();
            case Node::Type::Alt: {
                std::vector<std::uint32_t> jumps;
                for (std::size_t i = 0; i < node.children.size(); ++i) {
                    const bool last = i + 1 == node.children.size();
                    std::uint32_t split_at = 0;
                    if (!last) {
                        split_at = here();
                        inst = Regex::Inst();
                        inst.op = Op::Split;
                        cml::Result<void> r = push(inst);
                        if (!r) {
                            return r;
                        }
                        out_->program_[split_at].x = here();
                    }
                    cml::Result<void> r = emit(node.children[i]);
                    if (!r) {
                        return r;
                    }
                    if (!last) {
                        jumps.push_back(here());
                        inst = Regex::Inst();
                        inst.op = Op::Jump;
                        r = push(inst);
                        if (!r) {
                            return r;
                        }
                        out_->program_[split_at].y = here();
                    }
                }
                for (const std::uint32_t j : jumps) {
                    out_->program_[j].x = here();
                }
                return cml::Result<void>();
            }
            case Node::Type::Repeat: {
                const std::uint32_t child = node.children[0];
                for (int i = 0; i < node.min; ++i) {
                    cml::Result<void> r = emit(child);
                    if (!r) {
                        return r;
                    }
                }
                if (node.max == kInfinite) {
                    // L: split body, out ; body: child ; jump L ; out:
                    const std::uint32_t loop = here();
                    inst = Regex::Inst();
                    inst.op = Op::Split;
                    cml::Result<void> r = push(inst);
                    if (!r) {
                        return r;
                    }
                    const std::uint32_t body = here();
                    r = emit(child);
                    if (!r) {
                        return r;
                    }
                    inst = Regex::Inst();
                    inst.op = Op::Jump;
                    inst.x = loop;
                    r = push(inst);
                    if (!r) {
                        return r;
                    }
                    set_split(loop, body, here(), node.lazy);
                    return cml::Result<void>();
                }
                std::vector<std::uint32_t> splits;
                for (int i = node.min; i < node.max; ++i) {
                    splits.push_back(here());
                    inst = Regex::Inst();
                    inst.op = Op::Split;
                    cml::Result<void> r = push(inst);
                    if (!r) {
                        return r;
                    }
                    r = emit(child);
                    if (!r) {
                        return r;
                    }
                }
                for (const std::uint32_t s : splits) {
                    set_split(s, s + 1, here(), node.lazy);
                }
                return cml::Result<void>();
            }
        }
        return cml::Result<void>();
    }

    void set_split(std::uint32_t at, std::uint32_t body, std::uint32_t out, bool lazy) {
        Regex::Inst& s = out_->program_[at];
        s.x = lazy ? out : body;
        s.y = lazy ? body : out;
    }

    std::string_view pattern_;
    Regex* out_;
    std::size_t pos_ = 0;
    std::vector<Node> nodes_;
};

cml::Result<Regex> Regex::compile(std::string_view pattern) {
    Regex regex;
    regex.pattern_ = std::string(pattern);
    RegexCompiler compiler(regex.pattern_, &regex);
    cml::Result<void> r = compiler.run();
    if (!r) {
        return r.error();
    }
    return regex;
}

bool Regex::search(std::string_view text, std::size_t from, Match* match) const {
    budget_exceeded_ = false;
    std::size_t steps = 0;
    struct Frame {
        std::uint32_t pc;
        std::size_t pos;
    };
    std::vector<Frame> stack;
    for (std::size_t start = from; start <= text.size(); ++start) {
        stack.clear();
        stack.push_back(Frame{0, start});
        while (!stack.empty()) {
            Frame frame = stack.back();
            stack.pop_back();
            std::uint32_t pc = frame.pc;
            std::size_t pos = frame.pos;
            for (;;) {
                if (++steps > kMaxSteps) {
                    budget_exceeded_ = true;
                    return false;
                }
                const Inst& inst = program_[pc];
                bool ok = true;
                switch (inst.op) {
                    case Inst::Op::Char:
                        ok = pos < text.size() && text[pos] == inst.ch;
                        if (ok) {
                            ++pos;
                            ++pc;
                        }
                        break;
                    case Inst::Op::Any:
                        ok = pos < text.size() && text[pos] != '\n' && text[pos] != '\r';
                        if (ok) {
                            ++pos;
                            ++pc;
                        }
                        break;
                    case Inst::Op::Class:
                        ok = pos < text.size() && classes_[inst.x].bits[static_cast<unsigned char>(text[pos])];
                        if (ok) {
                            ++pos;
                            ++pc;
                        }
                        break;
                    case Inst::Op::Bol:
                        ok = pos == 0 || text[pos - 1] == '\n';
                        if (ok) ++pc;
                        break;
                    case Inst::Op::Eol:
                        ok = pos == text.size() || text[pos] == '\n' || text[pos] == '\r';
                        if (ok) ++pc;
                        break;
                    case Inst::Op::WordB:
                    case Inst::Op::NotWordB: {
                        const bool before = pos > 0 && is_word_char(static_cast<unsigned char>(text[pos - 1]));
                        const bool after = pos < text.size() && is_word_char(static_cast<unsigned char>(text[pos]));
                        ok = (before != after) == (inst.op == Inst::Op::WordB);
                        if (ok) ++pc;
                        break;
                    }
                    case Inst::Op::Split:
                        if (stack.size() >= kMaxStack) {
                            budget_exceeded_ = true;
                            return false;
                        }
                        stack.push_back(Frame{inst.y, pos});
                        pc = inst.x;
                        break;
                    case Inst::Op::Jump:
                        pc = inst.x;
                        break;
                    case Inst::Op::Match:
                        if (match != nullptr) {
                            match->begin = start;
                            match->end = pos;
                        }
                        return true;
                }
                if (!ok) {
                    break;  // backtrack: pop the next frame
                }
            }
        }
    }
    return false;
}

}  // namespace cmltest
