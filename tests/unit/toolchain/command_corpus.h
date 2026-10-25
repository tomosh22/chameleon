#pragma once

// Table of compile command lines for the WP-05 acceptance test (test_command_corpus.cpp).
//
// Each case is a RECORDED command (what cml_pdb::build_env() / compile_commands.json would give):
// the arguments without the program name, the compiler path, the cwd and the source. The test asserts
//   1. the tokenisation of `command` (tokens joined with '|'),
//   2. the rewritten command line (`<OUT>` stands for the patch object path), and
//   3. the diagnostics: ids of the warnings, or the id of the error that refuses the rewrite.
//
// Provenance (`kind`):
//   Real          copied from a public file or a CI capture; `source` names where
//   Constructed   written by hand from documented option syntax in the style of the named producer;
//                 NOT a capture. These exist to cover syntax the real ones do not (quoting corner
//                 cases, response files, refusals).
// Real lines from public repositories are short flag lists of generated build files; the licence of
// those repositories was not checked, so only the command lines (facts about a build) are used.
//
// Response files named in `command` come from kResponseFiles (the test's virtual file system).

#include <cstdint>

namespace cml_corpus {

enum class Kind { Real, Constructed };

struct Case {
    const char* id;
    Kind kind;
    const char* source_of_line;  // producer and where the line comes from
    const char* compiler;
    const char* cwd;
    const char* source;
    const char* command;
    int pch_mode;  // 0 = Auto (the library default), 1 = ForceZ7, 2 = KeepZiPrivatePdb, 3 = DropPch
    const char* expected_tokens;
    const char* expected_command;
    const char* expected_warnings;  // comma separated ids, "" = none
    unsigned expected_error;        // 0 = rewrite must succeed
};

struct VirtualFile {
    const char* path;
    const char* text;
};

// clang-format off
inline constexpr VirtualFile kResponseFiles[] = {
    {R"(D:\proj\build\opts.rsp)", "/DFROM_RSP=1\n/I\"D:\\proj\\inc dir\"\r\n/EHsc"},
    {R"(D:\proj\build\outer.rsp)", "@inner.rsp /DOUTER"},
    {R"(D:\proj\build\inner.rsp)", "/DINNER=2"},
    {R"(D:\proj\build\loop.rsp)", "@loop.rsp"},
};
// clang-format on

}  // namespace cml_corpus
