#pragma once

// A small backtracking regular-expression engine for `expect_stdout` patterns.
//
// Why not std::regex: the whole repository compiles with _HAS_EXCEPTIONS=0 (cmake/CmlToolchain.cmake),
// under which a bad pattern or a deep recursion in std::regex terminates the process instead of
// throwing. This engine reports a bad pattern as a Diag (CML5420), never recurses on the input
// (explicit stack) and is bounded by a step budget, so a pattern cannot hang the harness.
//
// Syntax (ECMAScript flavoured subset): literals, `.` (any character except newline), classes
// `[abc]` `[a-z]` `[^...]`, escapes `\d \D \w \W \s \S \b \B \t \n \r \. \\ ...`, groups `( ... )`
// and `(?: ... )` (both non-capturing: only the overall match is reported), alternation `|`,
// quantifiers `* + ? {n} {n,} {n,m}` with an optional lazy `?` suffix, anchors `^` and `$` (they
// match at line starts and ends, since the text is multi-line program output).
// Not supported: back-references, look-around, named groups, Unicode classes. Matching is on bytes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cml/util/result.h"

namespace cmltest {

class Regex {
public:
    struct Match {
        std::size_t begin = 0;
        std::size_t end = 0;
    };

    // CML5420 for a malformed pattern (the message names the position) or one that is too big.
    static cml::Result<Regex> compile(std::string_view pattern);

    // Leftmost match at or after `from`. Returns false when there is none; `budget_exceeded()` then
    // tells whether the search gave up because of the step budget (treated as "no match").
    bool search(std::string_view text, std::size_t from, Match* match) const;

    bool budget_exceeded() const { return budget_exceeded_; }
    const std::string& pattern() const { return pattern_; }

    struct Inst {
        enum class Op : std::uint8_t { Char, Any, Class, Split, Jump, Mark, LoopBack, Bol, Eol, WordB, NotWordB, Match };
        Op op = Op::Match;
        char ch = 0;
        std::uint32_t x = 0;  // Class: index into classes_; Split/Jump: target
        std::uint32_t y = 0;  // Split: second target (tried after x)
    };

    static constexpr std::size_t kMaxProgram = 20000;
    static constexpr std::size_t kMaxSteps = 20000000;  // budget of one search() call, over all start positions

private:
    struct ClassSet {
        bool bits[256];
    };

    friend class RegexCompiler;

    std::string pattern_;
    std::vector<Inst> program_;
    std::vector<ClassSet> classes_;
    std::uint32_t slots_ = 0;  // loop registers used by Mark/LoopBack
    mutable bool budget_exceeded_ = false;
};

}  // namespace cmltest
