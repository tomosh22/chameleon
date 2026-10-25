#include "cml/toolchain/rewrite.h"

#include <cstdio>
#include <memory>
#include <regex>
#include <vector>

#include "cml/pdb/index.h"
#include "cml/toolchain/diag_ids.h"
#include "cml/util/log.h"
#include "cml/util/path.h"
#include "fs_util.h"

namespace cml::toolchain {

namespace {

// ---- rule matching --------------------------------------------------------------------------

using RegexList = std::vector<std::unique_ptr<std::regex>>;

// Compiled regexes of one rule table (parallel to the rules; null for non-Regex rules), built once.
// Patterns are literals in rules.cpp.
RegexList compile_table(std::span<const Rule> rules) {
    RegexList list;
    for (const Rule& r : rules) {
        list.push_back(r.match == MatchKind::Regex ? std::make_unique<std::regex>(r.pattern) : nullptr);
    }
    return list;
}
const RegexList& cl_regexes() {
    static const RegexList list = compile_table(cl_rules());
    return list;
}
const RegexList& extra_regexes() {
    static const RegexList list = compile_table(clang_cl_extra_rules());
    return list;
}
const RegexList& cc1_regexes() {
    static const RegexList list = compile_table(cc1_rules());
    return list;
}

struct TableRef {
    std::span<const Rule> rules;
    const RegexList* regexes;
};

bool rule_matches(const Rule& rule, const std::regex* re, std::string_view text) {
    switch (rule.match) {
        case MatchKind::Exact: return text == rule.pattern;
        case MatchKind::Prefix: return detail::starts_with(text, rule.pattern);
        case MatchKind::Regex: {
            const std::string s(text);
            return re != nullptr && std::regex_match(s, *re);
        }
    }
    return false;
}

const Rule* find_rule(const std::vector<TableRef>& tables, std::string_view text) {
    for (const TableRef& t : tables) {
        for (std::size_t i = 0; i < t.rules.size(); ++i) {
            if (rule_matches(t.rules[i], (*t.regexes)[i].get(), text)) {
                return &t.rules[i];
            }
        }
    }
    return nullptr;
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

std::string what_to_do_for(std::uint32_t code) {
    switch (code) {
        case diag::kPchCreatingTu: return "Rebuild the project and hot-restart.";
        case diag::kUnsupportedOption: return "Build this module without that option, or rebuild the project and hot-restart.";
        default: return "Rebuild the project and hot-restart.";
    }
}

bool is_option(const std::string& tok) {
    return tok.size() >= 2 && (tok[0] == '-' || tok[0] == '/');
}

bool has_source_extension(std::string_view p) {
    const std::string l = detail::lower(p);
    for (const char* ext : {".cpp", ".cc", ".cxx", ".c++", ".c", ".cppm", ".ixx"}) {
        if (detail::ends_with(l, ext)) {
            return true;
        }
    }
    return false;
}

std::string path_stem(std::string_view source) {
    std::string_view base = path::base_name(source);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string_view::npos && dot != 0) {
        base = base.substr(0, dot);
    }
    return std::string(base);
}

struct Seen {
    bool debug_info = false;
    bool zi_replaced = false;
    bool pch_use = false;
    bool show_includes = false;
    bool codeview_cmdline = false;
    bool function_linking = false;
    bool emit_obj = false;
    std::string fd_value;
    std::size_t z7_index = static_cast<std::size_t>(-1);
    std::vector<std::size_t> pch_tokens;  // indices into the kept list of /Yu and /Fp tokens
};

}  // namespace

std::string patch_object_path(std::string_view session_tmp, std::string_view module_name, std::uint64_t compiland_hash,
                              std::string_view source) {
    std::string module;
    for (const char c : module_name) {
        module.push_back(std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos ? '_' : c);
    }
    if (module.empty()) {
        module = "module";
    }
    char hash[24];
    std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(compiland_hash));
    std::string out(session_tmp);
    if (!out.empty() && !path::is_separator(out.back())) {
        out.push_back('\\');
    }
    out += module;
    out.push_back('\\');
    out += hash;
    out.push_back('\\');
    out += path_stem(source);
    out += ".obj";
    return out;
}

Result<RewriteResult> rewrite_for_patch(const CompileRecord& record, const RewriteOptions& options) {
    RewriteResult res;
    res.dialect = detect_dialect(record);
    res.compiler = record.compiler;
    res.cwd = record.cwd;
    res.source = record.source;

    if (res.dialect == Dialect::Unknown) {
        return Diag::make(diag::kUnsupportedCompiler,
                          "compiler '" + record.compiler + "' is neither cl, clang-cl nor a clang -cc1 command",
                          "Chameleon can recompile code built by MSVC cl or clang-cl; rebuild with one of them.");
    }
    if (options.output_object.empty()) {
        return Diag::make(diag::kCannotPrepareOutput, "no output object path was given for '" + record.source + "'",
                          "This is a Chameleon bug; report it with the log.");
    }

    Result<std::vector<std::string>> expanded = expand_response_files(record.args, record.cwd, options.file_reader);
    if (!expanded) {
        return expanded.error();
    }
    res.expanded_args = *expanded;

    const bool cc1 = res.dialect == Dialect::ClangCc1;
    std::vector<TableRef> tables;
    if (cc1) {
        tables.push_back({cc1_rules(), &cc1_regexes()});
    } else {
        if (res.dialect == Dialect::ClangClDriver) {
            tables.push_back({clang_cl_extra_rules(), &extra_regexes()});
        }
        tables.push_back({cl_rules(), &cl_regexes()});
    }

    const auto trace = [&](const std::string& line) {
        res.trace.push_back(line);
        CML_LOG(Trace, "toolchain: rewrite %s: %s", record.source.c_str(), line.c_str());
    };

    // The source name for messages: the record's, else the last token that looks like a source file.
    std::string display_source = record.source;
    if (display_source.empty()) {
        for (const std::string& t : res.expanded_args) {
            if (!is_option(t) && has_source_extension(t)) {
                display_source = t;
            }
        }
    }

    Seen seen;
    std::vector<std::string> kept;
    const std::vector<std::string>& in = res.expanded_args;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::string& tok = in[i];
        if (!is_option(tok)) {
            kept.push_back(tok);
            trace("keep input " + tok);
            continue;
        }
        const std::string_view text = std::string_view(tok).substr(1);
        const Rule* rule = find_rule(tables, text);
        if (rule == nullptr) {
            kept.push_back(tok);
            continue;
        }
        bool already_warned = false;
        for (const Diag& w : res.warnings) {
            already_warned = already_warned || w.code == rule->warn_diag;
        }
        if (rule->warn_diag != 0 && !already_warned) {
            res.warnings.push_back(Diag::make(rule->warn_diag, replace_all(replace_all(rule->why, "{opt}", tok), "{file}", display_source),
                                              rule->warn_diag == diag::kWholeProgramDowngraded
                                                  ? "Build with /GL- for hot-reload sessions to keep the patched code identical to the original."
                                                  : "See the log for details."));
        }
        switch (rule->tag) {
            case Tag::DebugInfo: seen.debug_info = true; break;
            case Tag::EditAndContinue: seen.debug_info = true; break;
            case Tag::PchUse: seen.pch_use = true; break;
            case Tag::ShowIncludes: seen.show_includes = true; break;
            case Tag::CodeViewCommandLine: seen.codeview_cmdline = true; break;
            case Tag::FunctionLinking: seen.function_linking = true; break;
            case Tag::PdbFile: seen.fd_value = tok.substr(3); break;
            default: break;
        }
        if (rule->tag == Tag::PchUse || rule->tag == Tag::PchFile) {
            seen.pch_tokens.push_back(kept.size());
        }
        switch (rule->action) {
            case Action::Keep: {
                kept.push_back(tok);
                for (std::size_t k = 1; k <= rule->consume_next && i + k < in.size(); ++k) {
                    kept.push_back(in[i + k]);
                }
                i += rule->consume_next;
                trace("keep " + tok);
                break;
            }
            case Action::Drop: {
                trace("drop " + tok + " (" + rule->why + ")");
                i += rule->consume_next;
                break;
            }
            case Action::Replace: {
                if (rule->replacement[0] != '\0') {
                    if (rule->tag == Tag::DebugInfo || rule->tag == Tag::EditAndContinue) {
                        seen.zi_replaced = true;
                        seen.z7_index = kept.size();
                    }
                    kept.push_back(std::string(1, tok[0]) + rule->replacement);
                    trace("replace " + tok + " -> " + kept.back());
                } else {
                    trace("drop " + tok + " (" + rule->why + ")");
                }
                i += rule->consume_next;
                if (rule->tag == Tag::EditAndContinue && !seen.function_linking) {
                    // /ZI implied /Gy: keep function-level linking. A later explicit /Gy- is still
                    // honoured because it is kept verbatim after this point... except that the
                    // explicit option comes later in the list and wins in cl as well.
                    kept.push_back(std::string(1, tok[0]) + "Gy");
                    trace("add " + kept.back() + " (/ZI implied /Gy)");
                }
                break;
            }
            case Action::DropRest: {
                trace("drop " + tok + " and the rest (" + rule->why + ")");
                i = in.size();
                break;
            }
            case Action::Error: {
                const std::string msg = replace_all(replace_all(rule->why, "{opt}", tok), "{file}",
                                                    display_source.empty() ? std::string("<source>") : display_source);
                return Diag::make(rule->diag, msg, what_to_do_for(rule->diag));
            }
        }
    }

    // ---- locate the source ----------------------------------------------------------------
    const std::string want_source = record.source.empty() ? std::string() : pdb::normalize_path(record.source, record.cwd);
    bool source_present = false;
    std::string last_source_like;
    for (const std::string& t : kept) {
        if (!is_option(t)) {
            if (!want_source.empty() && pdb::normalize_path(t, record.cwd) == want_source) {
                source_present = true;
            }
            if (has_source_extension(t)) {
                last_source_like = t;
            }
        } else if (!cc1 && t.size() > 3 && (t.compare(1, 2, "Tp") == 0 || t.compare(1, 2, "Tc") == 0)) {
            if (!want_source.empty() && pdb::normalize_path(t.substr(3), record.cwd) == want_source) {
                source_present = true;
            }
        }
    }
    if (res.source.empty()) {
        res.source = last_source_like;
        source_present = !last_source_like.empty();
    }
    if (res.source.empty()) {
        return Diag::make(diag::kNoSourceFile, "the recorded command line names no source file",
                          "Provide the source path (compile_commands.json entry or a PDB with a source record).");
    }

    // ---- the PCH / debug-info interplay (cl) ------------------------------------------------
    PchDebugMode mode = options.pch_debug == PchDebugMode::Auto ? kDefaultPchDebugMode : options.pch_debug;
    res.uses_pch = seen.pch_use;
    res.debug_info_replaced = seen.zi_replaced;
    const bool cl_syntax = !cc1;
    std::string private_pdb;
    if (seen.pch_use && cl_syntax && res.dialect == Dialect::Cl) {
        if (mode == PchDebugMode::DropPch) {
            // Remove /Yu and /Fp (highest index first so the indices stay valid).
            for (std::size_t k = seen.pch_tokens.size(); k > 0; --k) {
                const std::size_t idx = seen.pch_tokens[k - 1];
                if (idx < kept.size()) {
                    trace("drop " + kept[idx] + " (PchDebugMode::DropPch)");
                    kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(idx));
                    if (seen.z7_index != static_cast<std::size_t>(-1) && idx < seen.z7_index) {
                        --seen.z7_index;
                    }
                }
            }
        } else if (mode == PchDebugMode::KeepZiPrivatePdb && seen.zi_replaced && seen.z7_index < kept.size()) {
            kept[seen.z7_index] = std::string(1, kept[seen.z7_index][0]) + "Zi";
            std::string dir = options.private_pdb_dir;
            if (dir.empty()) {
                dir = std::string(path::parent_directory(std::string_view(options.output_object)));
            }
            private_pdb = dir + (dir.empty() || path::is_separator(dir.back()) ? "" : "\\") + path_stem(res.source) + ".compiler.pdb";
            std::string from = seen.fd_value.empty() ? record.recorded_pdb : seen.fd_value;
            if (!from.empty() && !detail::is_absolute(from) && !record.cwd.empty()) {
                from = record.cwd + (path::is_separator(record.cwd.back()) ? "" : "\\") + from;
            }
            if (from.empty()) {
                res.warnings.push_back(Diag::make(diag::kPdbCopyFailed,
                                                  "no compiler PDB is recorded for '" + res.source + "'; the PCH-using TU is compiled with a fresh /Zi PDB",
                                                  "If the compile fails with C2859, build the project with /Z7 or set PchDebugMode::DropPch."));
            } else {
                res.pdb_copy_from = from;
                res.pdb_copy_to = private_pdb;
            }
            trace("keep /Zi with private PDB " + private_pdb + " (PchDebugMode::KeepZiPrivatePdb)");
            seen.zi_replaced = false;
            res.debug_info_replaced = false;
        }
    }

    // ---- assemble ---------------------------------------------------------------------------
    std::vector<std::string>& out = res.args;
    if (cc1) {
        out.insert(out.end(), kept.begin(), kept.end());  // starts with -cc1 (that is how the dialect is detected)
        for (const std::string& t : out) {
            if (t == "-emit-obj") {
                seen.emit_obj = true;
            }
        }
        if (!seen.emit_obj) {
            out.push_back("-emit-obj");
        }
        bool has_gcodeview = false;
        for (const std::string& t : out) {
            if (t == "-gcodeview") {
                has_gcodeview = true;
            }
        }
        if (options.ensure_codeview_command_line && has_gcodeview && !seen.codeview_cmdline) {
            out.push_back("-gcodeview-command-line");
            trace("add -gcodeview-command-line");
        }
        if (options.capture_includes && !seen.show_includes) {
            out.push_back("-show-includes");
        }
        out.push_back("-o");
        out.push_back(options.output_object);
        if (!source_present) {
            out.push_back(res.source);
        }
    } else {
        out.push_back("/nologo");
        out.push_back("/c");
        out.insert(out.end(), kept.begin(), kept.end());
        if (options.capture_includes && !seen.show_includes) {
            out.push_back("/showIncludes");
        }
        if (!private_pdb.empty()) {
            out.push_back("/Fd" + private_pdb);
        }
        if (options.ensure_codeview_command_line && res.dialect == Dialect::ClangClDriver && seen.debug_info &&
            !seen.codeview_cmdline) {
            out.push_back("/clang:-gcodeview-command-line");
            trace("add /clang:-gcodeview-command-line");
        }
        out.push_back("/Fo" + options.output_object);
        if (!source_present) {
            out.push_back(res.source);
        }
    }
    res.command_line = join_command_line(out);
    CML_LOG(Debug, "toolchain: patch command for %s: %s %s", res.source.c_str(), res.compiler.c_str(), res.command_line.c_str());
    return res;
}

}  // namespace cml::toolchain
