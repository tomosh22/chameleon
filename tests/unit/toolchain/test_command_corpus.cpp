// WP-05 acceptance (1): table-driven tests over real-world compile command lines.
// For every case: the tokenisation, the rewritten command and the diagnostics are asserted.
#include <map>
#include <string>
#include <vector>

#include "cml/toolchain/toolchain.h"
#include "command_corpus.h"
#include "doctest.h"

using namespace cml::toolchain;

namespace {

using cml_corpus::Case;
using cml_corpus::Kind;

constexpr Case kCases[] = {
#include "command_corpus_cases.inc"
#include "command_corpus_ci.inc"
};

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string join(const std::vector<std::string>& v, char sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) {
            out.push_back(sep);
        }
        out += v[i];
    }
    return out;
}

FileReader virtual_files() {
    return [](const std::string& path) -> cml::Result<std::string> {
        for (const cml_corpus::VirtualFile& f : cml_corpus::kResponseFiles) {
            if (path == f.path) {
                return decode_response_file_bytes(f.text);
            }
        }
        return cml::Diag::make(diag::kResponseFileUnreadable, "cannot read response file '" + path + "'", "test file system");
    };
}

PchDebugMode mode_of(int m) {
    switch (m) {
        case 1: return PchDebugMode::ForceZ7;
        case 2: return PchDebugMode::KeepZiPrivatePdb;
        case 3: return PchDebugMode::DropPch;
        default: return PchDebugMode::Auto;
    }
}

constexpr const char* kOut = "C:\\cml\\out\\x.obj";

}  // namespace

TEST_CASE("corpus: at least 40 command lines, from every producer the plan names, with unique ids") {
    constexpr std::size_t n = sizeof(kCases) / sizeof(kCases[0]);
    CHECK(n >= 40);
    std::map<std::string, int> ids;
    int msbuild = 0, ninja = 0, fastbuild = 0, ubt = 0, cc1 = 0, driver = 0, real = 0;
    for (const Case& c : kCases) {
        CHECK(++ids[c.id] == 1);
        const std::string id = c.id;
        const std::string src = c.source_of_line;
        msbuild += (id.find("msbuild") != std::string::npos || src.find("MSBuild") != std::string::npos) ? 1 : 0;
        ninja += (id.find("ninja") != std::string::npos) ? 1 : 0;
        fastbuild += id.find("fastbuild") != std::string::npos ? 1 : 0;
        ubt += id.find("ubt") != std::string::npos ? 1 : 0;
        cc1 += id.find("cc1") != std::string::npos ? 1 : 0;
        driver += id.find("driver") != std::string::npos ? 1 : 0;
        real += c.kind == Kind::Real ? 1 : 0;
    }
    CHECK(msbuild >= 3);
    CHECK(ninja >= 3);
    CHECK(fastbuild >= 3);
    CHECK(ubt >= 2);
    CHECK(cc1 >= 3);
    CHECK(driver >= 1);
    CHECK(real >= 2);
}

TEST_CASE("corpus: tokenisation, rewritten command and diagnostics of every case") {
    for (const Case& c : kCases) {
        CAPTURE(c.id);
        CompileRecord rec;
        rec.compiler = c.compiler;
        rec.cwd = c.cwd;
        rec.source = c.source;
        rec.args = tokenize_recorded_command(c.command, c.from_pdb != 0, c.source);

        // 1. tokenisation
        CHECK(join(rec.args, '|') == std::string(c.expected_tokens));

        // 2. + 3. rewrite and diagnostics
        RewriteOptions options;
        options.output_object = kOut;
        options.pch_debug = mode_of(c.pch_mode);
        options.file_reader = virtual_files();
        const cml::Result<RewriteResult> r = rewrite_for_patch(rec, options);
        if (c.expected_error != 0) {
            REQUIRE_FALSE(r);
            CHECK(r.error().code == c.expected_error);
            CHECK(r.error().valid());
            CHECK_FALSE(r.error().what_to_do.empty());
            if (c.expected_error == diag::kPchCreatingTu) {
                // The message pre-allocated in docs/KNOWN_LIMITATIONS.md.
                CHECK(r.error().message.find("creates the precompiled header (/Yc); changes to it cannot be hot-reloaded.") !=
                      std::string::npos);
            }
            continue;
        }
        REQUIRE(r);
        std::string command = replace_all(r->command_line, kOut, "<OUT>");
        command = replace_all(command, "C:\\cml\\out\\", "<OUTDIR>\\");
        CHECK(command == std::string(c.expected_command));
        std::vector<std::string> warnings;
        for (const cml::Diag& w : r->warnings) {
            warnings.push_back(std::to_string(w.code));
            CHECK(w.valid());
        }
        CHECK(join(warnings, ',') == std::string(c.expected_warnings));
        // Invariants of every successful rewrite.
        CHECK(command.find("<OUT>") != std::string::npos);
    }
}

TEST_CASE("corpus: the rewritten command line tokenises back to the rewritten arguments") {
    for (const Case& c : kCases) {
        CAPTURE(c.id);
        CompileRecord rec;
        rec.compiler = c.compiler;
        rec.cwd = c.cwd;
        rec.source = c.source;
        rec.args = tokenize_recorded_command(c.command, c.from_pdb != 0, c.source);
        RewriteOptions options;
        options.output_object = kOut;
        options.pch_debug = mode_of(c.pch_mode);
        options.file_reader = virtual_files();
        const cml::Result<RewriteResult> r = rewrite_for_patch(rec, options);
        if (!r) {
            continue;
        }
        CHECK(split_command_line(r->command_line) == r->args);
    }
}
