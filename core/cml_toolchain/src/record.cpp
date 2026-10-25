#include "cml/toolchain/record.h"

#include "cml/toolchain/command_line.h"
#include "cml/toolchain/diag_ids.h"
#include "cml/util/log.h"
#include "cml/util/path.h"
#include "fs_util.h"

namespace cml::toolchain {

const char* to_string(RecordOrigin origin) noexcept {
    switch (origin) {
        case RecordOrigin::Manual: return "manual";
        case RecordOrigin::PdbEnvBlock: return "S_ENVBLOCK";
        case RecordOrigin::PdbBuildInfo: return "S_BUILDINFO";
        case RecordOrigin::CompileCommands: return "compile_commands.json";
    }
    return "?";
}

const char* to_string(Dialect dialect) noexcept {
    switch (dialect) {
        case Dialect::Unknown: return "unknown";
        case Dialect::Cl: return "cl";
        case Dialect::ClangClDriver: return "clang-cl";
        case Dialect::ClangCc1: return "clang-cc1";
    }
    return "?";
}

std::string lower_base_name(std::string_view path) {
    return detail::lower(path::base_name(path));
}

namespace {

bool names_source(const std::string& token, std::string_view source) {
    if (source.empty()) {
        return false;
    }
    return path::same_path(token, source) || path::same_path(path::base_name(std::string_view(token)), path::base_name(source));
}

// Glues the pieces of unquoted paths with spaces back together (see tokenize_recorded_command).
std::vector<std::string> repair_unquoted_paths(std::vector<std::string> tokens, std::string_view source) {
    std::vector<std::string> out;
    for (std::string& t : tokens) {
        const bool option_like = !t.empty() && (t[0] == '-' || t[0] == '/' || t[0] == '@');
        if (!option_like && !t.empty() && !out.empty() && !names_source(t, source)) {
            out.back() += ' ';
            out.back() += t;
        } else {
            out.push_back(std::move(t));
        }
    }
    return out;
}

}  // namespace

std::vector<std::string> tokenize_recorded_command(std::string_view command, bool pdb_record, std::string_view source) {
    std::string_view head = command;
    while (!head.empty() && (head.front() == ' ' || head.front() == '\t')) {
        head.remove_prefix(1);
    }
    if (head.substr(0, 7) == "\"-cc1\" " || head.substr(0, 5) == "-cc1 " || head == "\"-cc1\"" || head == "-cc1") {
        return split_command_line_posix(command);
    }
    std::vector<std::string> tokens = split_command_line(command);
    return pdb_record ? repair_unquoted_paths(std::move(tokens), source) : tokens;
}

Result<CompileRecord> record_from_build_env(const pdb::BuildEnv& env) {
    CompileRecord r;
    switch (env.source) {
        case pdb::EnvSource::EnvBlock: r.origin = RecordOrigin::PdbEnvBlock; break;
        case pdb::EnvSource::BuildInfo: r.origin = RecordOrigin::PdbBuildInfo; break;
        case pdb::EnvSource::None: break;
    }
    const auto get = [&](std::string_view key) {
        const auto v = env.get(key);
        return v ? std::string(*v) : std::string();
    };
    r.cwd = get("cwd");
    r.compiler = get("cl");
    r.source = get("src");
    r.recorded_pdb = get("pdb");
    const std::string cmd = get("cmd");
    if (env.source == pdb::EnvSource::None || cmd.empty()) {
        return Diag::make(diag::kNoCompileCommand,
                          "compiland '" + (r.source.empty() ? std::string("?") : r.source) +
                              "' has no recorded command line",
                          "Build with MSVC /Zi or /Z7 (clang-cl: -gcodeview-command-line) or provide compile_commands.json.");
    }
    r.args = tokenize_recorded_command(cmd, true, r.source);
    if (!r.args.empty()) {
        const std::string first = detail::lower(r.args.front());
        const bool looks_like_program = !r.args.front().empty() && r.args.front()[0] != '-' && r.args.front()[0] != '/' &&
                                        r.args.front()[0] != '@' && detail::ends_with(first, ".exe");
        if (looks_like_program || (!r.compiler.empty() && path::same_path(r.args.front(), r.compiler))) {
            if (r.compiler.empty()) {
                r.compiler = r.args.front();
            }
            r.args.erase(r.args.begin());
        }
    }
    CML_LOG(Trace, "toolchain: record from %s: cwd='%s' compiler='%s' source='%s' %zu args", to_string(r.origin),
            r.cwd.c_str(), r.compiler.c_str(), r.source.c_str(), r.args.size());
    return r;
}

Dialect detect_dialect(const CompileRecord& record) noexcept {
    if (!record.args.empty() && (record.args.front() == "-cc1" || record.args.front() == "--cc1")) {
        return Dialect::ClangCc1;
    }
    const std::string name = lower_base_name(record.compiler);
    if (name == "cl.exe" || name == "cl") {
        return Dialect::Cl;
    }
    if (name == "clang-cl.exe" || name == "clang-cl") {
        return Dialect::ClangClDriver;
    }
    return Dialect::Unknown;
}

}  // namespace cml::toolchain
