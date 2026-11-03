#include "cml/core/module_audit.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

#include "cml/core/module_index.h"
#include "cml/pe/pe.h"
#include "cml/toolchain/build_flags.h"
#include "cml/toolchain/command_line.h"
#include "cml/toolchain/link_patch.h"
#include "cml/toolchain/record.h"
#include "cml/util/format.h"
#include "cml/util/log.h"
#include "cml/util/path.h"

namespace cml::core {

namespace ad = audit_diag;
namespace fs = std::filesystem;

const char* to_string(Verdict verdict) noexcept {
    switch (verdict) {
        case Verdict::Pass: return "PASS";
        case Verdict::Warn: return "WARN";
        case Verdict::Fail: return "FAIL";
    }
    return "?";
}

std::size_t ModuleAudit::count(Verdict verdict) const noexcept {
    return static_cast<std::size_t>(std::count_if(findings.begin(), findings.end(), [&](const AuditFinding& f) { return f.verdict == verdict; }));
}

const AuditFinding* ModuleAudit::first_failure() const noexcept {
    for (const AuditFinding& f : findings) {
        if (f.verdict == Verdict::Fail) {
            return &f;
        }
    }
    return nullptr;
}

const AuditFinding* ModuleAudit::find(const std::string& check) const noexcept {
    for (const AuditFinding& f : findings) {
        if (f.check == check) {
            return &f;
        }
    }
    return nullptr;
}

namespace {

std::string detail_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string base_of(std::string_view path) {
    return std::string(path::base_name(path));
}

// "a.obj, b.obj, c.obj and 4 more"
std::string list_names(const std::vector<std::string>& names, std::size_t shown = 3) {
    std::string out;
    for (std::size_t i = 0; i < names.size() && i < shown; ++i) {
        out += (i == 0 ? "" : ", ") + names[i];
    }
    if (names.size() > shown) {
        out += " and " + std::to_string(names.size() - shown) + " more";
    }
    return out;
}

AuditFinding make(const char* check, Verdict verdict, std::uint32_t id, std::string message, std::string what_to_do, std::string evidence) {
    AuditFinding f;
    f.check = check;
    f.verdict = verdict;
    f.id = id;
    f.message = std::move(message);
    f.what_to_do = std::move(what_to_do);
    f.evidence = std::move(evidence);
    return f;
}

AuditFinding from_diag(const char* check, const Diag& d) {
    return make(check, Verdict::Fail, d.code, d.message, d.what_to_do, "raised by the reader: " + d.format());
}

std::vector<std::byte> read_object(const std::string& path, std::uint64_t cap) {
    std::error_code ec;
    const std::uint64_t size = fs::file_size(fs::path(path), ec);
    if (ec || size == 0 || size > cap) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<std::uint64_t>(in.gcount()) != size) {
        return {};
    }
    return bytes;
}

bool is_codeview_language_with_code(std::uint8_t language) {
    // MASM (3), LINK (7), CVTRES (8) and CVTPGD (9) compilands are not translation units we recompile.
    return !(language == 3 || language == 7 || language == 8 || language == 9);
}

}  // namespace

// ---- link options -----------------------------------------------------------------------------------------------

LinkOptionsFacts parse_link_options(const std::string& linker_command) {
    LinkOptionsFacts out;
    if (linker_command.empty()) {
        return out;
    }
    bool debug_seen = false;
    std::optional<bool> ref;
    std::optional<bool> icf;
    bool options_seen = false;
    for (const std::string& tok : toolchain::split_command_line(linker_command)) {
        if (tok.size() < 2 || (tok[0] != '/' && tok[0] != '-')) {
            continue;
        }
        options_seen = true;
        const std::string body = upper(std::string_view(tok).substr(1));
        if (body == "DEBUG" || body.compare(0, 6, "DEBUG:") == 0) {
            // link.exe: the last /DEBUG wins ("/DEBUG:FASTLINK ... /DEBUG:FULL" is a full PDB); /DEBUG:NONE turns debug info off.
            const std::string value = body.size() > 6 ? body.substr(6) : "";
            if (value == "NONE") {
                debug_seen = false;
                out.debug_fastlink.reset();
            } else {
                debug_seen = true;
                out.debug_fastlink = (value == "FASTLINK");
            }
        } else if (body.compare(0, 4, "OPT:") == 0) {
            std::size_t pos = 4;
            while (pos <= body.size()) {
                const std::size_t comma = body.find(',', pos);
                const std::string item = body.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (item == "REF") ref = true;
                else if (item == "NOREF") ref = false;
                else if (item == "ICF" || item.compare(0, 4, "ICF=") == 0 || item == "SAFEICF") icf = true;
                else if (item == "NOICF") icf = false;
                if (comma == std::string::npos) {
                    break;
                }
                pos = comma + 1;
            }
        } else if (body == "FUNCTIONPADMIN" || body.compare(0, 15, "FUNCTIONPADMIN:") == 0) {
            out.function_pad_min = true;
        } else if (body == "GUARD:CF" || body.compare(0, 9, "GUARD:CF,") == 0) {
            out.guard_cf = true;
        }
    }
    out.known = options_seen;
    // The linker's defaults: with /DEBUG, /OPT:REF and /OPT:ICF are off unless asked for; without it they are on.
    out.opt_ref = ref.value_or(!debug_seen);
    out.opt_icf = icf.value_or(!debug_seen);
    out.detail = std::string("linker command: /DEBUG ") + (debug_seen ? (out.debug_fastlink.value_or(false) ? "FASTLINK" : "FULL") : "not seen") +
                 ", /OPT:" + (out.opt_ref ? "REF" : "NOREF") + (ref ? "" : "(default)") + "," + (out.opt_icf ? "ICF" : "NOICF") + (icf ? "" : "(default)") +
                 ", /FUNCTIONPADMIN " + (out.function_pad_min ? "yes" : "no");
    return out;
}

bool command_allows_inlining(const std::string& compile_command) {
    const pdb::OptimizationHint hint = pdb::optimization_hint_from_command(compile_command);
    if (hint == pdb::OptimizationHint::Unknown || hint == pdb::OptimizationHint::Disabled) {
        return false;
    }
    for (const std::string& tok : toolchain::tokenize_recorded_command(compile_command)) {
        if (tok == "/Ob0" || tok == "-Ob0" || tok == "-fno-inline" || tok == "-fno-inline-functions") {
            return false;
        }
    }
    return true;
}

std::optional<Diag> stale_inline_warning(const pdb::PdbIndex& index, std::uint32_t compiland, const std::vector<std::string>& changed_small,
                                         const std::string& module_name) {
    if (changed_small.empty() || compiland >= index.compiland_count()) {
        return std::nullopt;
    }
    Result<pdb::BuildEnv> env = index.build_env(compiland);
    if (!env) {
        return std::nullopt;
    }
    const auto command = env->command();
    if (!command || !command_allows_inlining(std::string(*command))) {
        return std::nullopt;
    }
    return Diag::make(ad::kStaleInlinedCopies,
                      "Compiland " + base_of(index.compiland(compiland).object_name) + " of " + module_name + " is optimised with inlining and " +
                          std::to_string(changed_small.size()) + " small changed function(s) (" + list_names(changed_small) +
                          ") may be inlined into their callers: a caller that is still executing, or whose own code did not change, keeps the old code.",
                      "Build iterated modules with /Ob0 or /Od, or hot-restart.");
}

// ---- the audit --------------------------------------------------------------------------------------------------

namespace {

struct CompilandSummary {
    std::vector<std::string> gl, guard_cf, gy_no, gy_unknown, managed, no_command, inlining;
    std::size_t gy_yes = 0;
    std::size_t with_command = 0;
    std::size_t hotpatch_bit = 0;
};

void examine_compilands(const pdb::PdbIndex& index, const AuditOptions& options, ModuleAudit& audit, CompilandSummary& s) {
    std::uint64_t object_budget = options.object_budget_bytes;
    for (std::uint32_t ci = 0; ci < index.compiland_count(); ++ci) {
        if (!ModuleIndex::is_source_compiland(index, ci)) {
            continue;
        }
        const pdb::Compiland& c = index.compiland(ci);
        // An object without S_COMPILE3 still counts (an IL object of a /GL build may have none); other record kinds
        // (MASM, resources, ...) do not.
        const std::string_view object_name = c.object_name;
        const bool object_file = object_name.size() > 4 && (object_name.substr(object_name.size() - 4) == ".obj" || object_name.substr(object_name.size() - 2) == ".o");
        if (c.has_compile3 ? !is_codeview_language_with_code(c.compile.language) : !object_file) {
            continue;
        }
        if (audit.compilands_examined >= options.max_compilands) {
            continue;
        }
        ++audit.compilands_examined;
        const std::string name = base_of(c.object_name);
        if (c.compile.managed || c.compile.msil_module) {
            s.managed.push_back(name);
        }
        if (c.compile.hotpatch) {
            ++s.hotpatch_bit;
        }

        // The recorded command line (S_ENVBLOCK, else S_BUILDINFO).
        std::vector<std::string> args;
        toolchain::Dialect dialect = toolchain::Dialect::Unknown;
        bool have_command = false;
        std::string cwd;
        std::string command_text;
        Result<pdb::BuildEnv> env = index.build_env(ci);
        if (env && env->source != pdb::EnvSource::None) {
            const auto cmd = env->command();
            if (cmd && !cmd->empty()) {
                Result<toolchain::CompileRecord> record = toolchain::record_from_build_env(*env);
                if (record) {
                    have_command = true;
                    command_text = std::string(*cmd);
                    cwd = record->cwd;
                    dialect = toolchain::detect_dialect(*record);
                    Result<std::vector<std::string>> expanded = toolchain::expand_response_files(record->args, record->cwd, toolchain::default_file_reader());
                    args = expanded ? std::move(*expanded) : record->args;
                }
            }
        }
        if (have_command) {
            ++s.with_command;
            if (command_allows_inlining(command_text)) {
                s.inlining.push_back(name);
            }
        } else {
            s.no_command.push_back(name);
        }

        // The recorded object, when it is still on disk (/GL and /Gy evidence).
        std::vector<std::byte> object;
        if (object_budget > 0) {
            std::string object_path(c.object_name);
            if (!pdb::is_absolute_path(object_path) && !cwd.empty()) {
                object_path = cwd + "\\" + object_path;
            }
            object = read_object(object_path, std::min(options.max_object_bytes, object_budget));
            object_budget -= std::min<std::uint64_t>(object_budget, object.size());
        }

        toolchain::CompilandFlagInputs in;
        in.args = args;
        in.dialect = dialect;
        in.have_command_line = have_command;
        in.compile3 = c.has_compile3 ? &c.compile : nullptr;
        in.object_bytes = object;
        const toolchain::CompilandFlagFacts facts = toolchain::detect_compiland_flags(in);
        if (facts.whole_program_optimization.value == toolchain::Tri::Yes) {
            s.gl.push_back(name);
        }
        if (facts.guard_cf.value == toolchain::Tri::Yes) {
            s.guard_cf.push_back(name);
        }
        switch (facts.function_level_linking.value) {
            case toolchain::Tri::Yes: ++s.gy_yes; break;
            case toolchain::Tri::No: s.gy_no.push_back(name); break;
            case toolchain::Tri::Unknown: s.gy_unknown.push_back(name); break;
        }
    }
}

// Samples compilands that contributed code and looks for a function symbol in each.
struct ProcSample {
    std::size_t sampled = 0;
    std::size_t found = 0;
};

ProcSample sample_function_symbols(const pdb::PdbIndex& index, const AuditOptions& options) {
    ProcSample out;
    const std::vector<pdb::SectionHeader>& sections = index.sections();
    std::set<std::uint32_t> seen;
    const std::size_t count = std::min<std::size_t>(index.contribution_count(), 200000);
    for (std::size_t i = 0; i < count && out.sampled < options.max_code_samples; ++i) {
        const pdb::SectionContribution c = index.contribution(i);
        if (c.compiland == pdb::kNoCompiland || c.rva == pdb::kNoRva || c.section == 0 || c.section > sections.size() || c.size == 0) {
            continue;
        }
        if ((sections[c.section - 1u].characteristics & 0x20u) == 0) {  // IMAGE_SCN_CNT_CODE
            continue;
        }
        if (!ModuleIndex::is_source_compiland(index, c.compiland)) {
            continue;
        }
        const pdb::Compiland& comp = index.compiland(c.compiland);
        if (!comp.has_compile3 || !is_codeview_language_with_code(comp.compile.language) || !seen.insert(c.compiland).second) {
            continue;
        }
        ++out.sampled;
        // A compiland that contributed code must list at least one function (S_GPROC32 / S_LPROC32) in a full PDB.
        Result<std::vector<pdb::SymbolView>> symbols = index.compiland_symbols(c.compiland);
        if (symbols && std::any_of(symbols->begin(), symbols->end(), [](const pdb::SymbolView& v) { return pdb::is_proc_kind(v.kind); })) {
            ++out.found;
        }
    }
    return out;
}

struct PaddingSample {
    std::size_t sampled = 0;
    std::size_t padded = 0;
};

PaddingSample sample_padding(const pdb::PdbIndex& index, const pe::Image& image, const AuditOptions& options) {
    PaddingSample out;
    Result<std::vector<pdb::SymbolView>> publics = index.all_publics();
    if (!publics) {
        return out;
    }
    std::vector<std::uint32_t> rvas;
    for (const pdb::SymbolView& s : *publics) {
        if (!s.is_function() || s.rva == pdb::kNoRva || s.rva < 6) {
            continue;
        }
        const std::optional<std::uint32_t> owner = index.compiland_for_rva(s.rva);
        if (owner && ModuleIndex::is_source_compiland(index, *owner)) {
            rvas.push_back(s.rva);
        }
    }
    std::sort(rvas.begin(), rvas.end());
    rvas.erase(std::unique(rvas.begin(), rvas.end()), rvas.end());
    if (rvas.empty()) {
        return out;
    }
    const std::size_t want = std::min<std::size_t>(options.max_functions_sampled, rvas.size());
    for (std::size_t k = 0; k < want; ++k) {
        const std::uint32_t rva = rvas[k * rvas.size() / want];
        Result<pe::ByteSpan> before = image.read_rva(rva - 6, 6);
        if (!before) {
            continue;  // first bytes of a section
        }
        ++out.sampled;
        bool all_padding = true;
        for (const std::byte b : *before) {
            all_padding = all_padding && (b == std::byte{0xCC} || b == std::byte{0x90});
        }
        if (all_padding) {
            ++out.padded;
        }
    }
    return out;
}

void audit_with_pdb(const std::string& module_path, const pdb::PdbIndex& index, const pe::Image& image, const AuditOptions& options, ModuleAudit& audit) {
    const std::string module_name = base_of(module_path);
    audit.pdb_path = index.path();
    audit.compilands_total = index.compiland_count();

    // ---- the linker's command line ---------------------------------------------------------------------------------
    const toolchain::PdbLinkerFacts linker = toolchain::read_pdb_linker_facts(index);
    const LinkOptionsFacts lo = parse_link_options(linker.command);

    // ---- PDB machine ------------------------------------------------------------------------------------------------
    // (the PE machine was checked by the caller)
    if (index.machine() != 0 && index.machine() != 0x8664) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "0x%04X", static_cast<unsigned>(index.machine()));
        audit.findings.push_back(make("image", Verdict::Fail, ad::kNotX64, "The PDB of " + module_name + " is for machine " + buf + "; only x64 is supported in v1.",
                                      "Use an x64 build.", "DBI machine type"));
        return;
    }

    // ---- /DEBUG:FULL ----------------------------------------------------------------------------------------------
    // The PDB itself is the evidence, the linker's command line only the fallback: a command line can say FASTLINK and be
    // overridden by a later /DEBUG:FULL, and lld-link writes a full PDB whatever it is given.
    const ProcSample procs = sample_function_symbols(index, options);
    {
        const std::string exe_lower = detail_lower(linker.exe_path);
        const std::string version_lower = detail_lower(linker.compile_version);
        const bool lld = exe_lower.find("lld") != std::string::npos || version_lower.find("lld") != std::string::npos ||
                         version_lower.find("llvm") != std::string::npos;
        std::string evidence = lo.known ? lo.detail : "the PDB records no linker command line";
        evidence += std::string("; linker: ") + (lld ? "lld-link" : "link.exe or unknown");
        evidence += "; function symbols found for " + std::to_string(procs.found) + " of " + std::to_string(procs.sampled) + " sampled code contributions";
        if (procs.sampled > 0 && procs.found == 0) {
            audit.findings.push_back(make("debug-info", Verdict::Fail, ad::kFastlinkPdb,
                                          "The PDB of " + module_name + " holds no function symbols for its compilands (a /DEBUG:FASTLINK or stripped PDB); Chameleon needs /DEBUG:FULL.",
                                          "Relink the module with /DEBUG:FULL.", evidence));
        } else if (procs.sampled == 0 && lo.debug_fastlink.value_or(false) && !lld) {
            audit.findings.push_back(make("debug-info", Verdict::Fail, ad::kFastlinkPdb,
                                          "Module " + module_name + " uses a /DEBUG:FASTLINK PDB; Chameleon needs /DEBUG:FULL.",
                                          "Relink the module with /DEBUG:FULL.", evidence + " (no function symbols could be sampled: the command line decides)"));
        } else {
            audit.findings.push_back(make("debug-info", Verdict::Pass, ad::kFastlinkPdb, "The PDB of " + module_name + " is a full PDB.", "", evidence));
        }
    }

    // ---- compilands ---------------------------------------------------------------------------------------------------
    CompilandSummary s;
    examine_compilands(index, options, audit, s);
    const std::string examined = std::to_string(audit.compilands_examined) + " source compilands examined";

    if (audit.compilands_examined == 0) {
        audit.findings.push_back(make("compile-command", Verdict::Fail, ad::kNoCompileCommand,
                                      "The PDB of " + module_name + " has no compilands that Chameleon can recompile (no S_COMPILE3 records of C/C++ objects).",
                                      "Build the module with MSVC /Zi, /ZI or /Z7 (clang-cl: /Z7), and link with /DEBUG:FULL.", examined));
        return;
    }
    if (s.with_command == 0) {
        audit.findings.push_back(make("compile-command", Verdict::Fail, ad::kNoCompileCommand,
                                      "Compiland " + s.no_command.front() + " of " + module_name + " has no recorded command line; it cannot be reloaded" +
                                          (s.no_command.size() > 1 ? " (nor can " + std::to_string(s.no_command.size() - 1) + " others)." : "."),
                                      "Build with MSVC /Zi or /Z7 (clang-cl: -gcodeview-command-line) or provide compile_commands.json.",
                                      examined + "; neither S_ENVBLOCK nor S_BUILDINFO carries a command line"));
    } else if (!s.no_command.empty()) {
        audit.findings.push_back(make("compile-command", Verdict::Warn, ad::kNoCompileCommand,
                                      std::to_string(s.no_command.size()) + " of " + std::to_string(audit.compilands_examined) + " compilands of " + module_name +
                                          " have no recorded command line and cannot be reloaded (" + list_names(s.no_command) + ").",
                                      "Build them with MSVC /Zi or /Z7 (clang-cl: -gcodeview-command-line) or provide compile_commands.json.",
                                      examined + "; " + std::to_string(s.with_command) + " have a command line"));
    } else {
        audit.findings.push_back(make("compile-command", Verdict::Pass, ad::kNoCompileCommand, "Every source compiland of " + module_name + " records its command line.", "",
                                      examined));
    }

    if (!s.gl.empty()) {
        audit.findings.push_back(make("gl", Verdict::Fail, ad::kWholeProgram,
                                      "Compiland " + s.gl.front() + " in " + module_name + " was built with /GL (LTCG); its objects are IL, not COFF" +
                                          (s.gl.size() > 1 ? " (" + std::to_string(s.gl.size()) + " compilands: " + list_names(s.gl) + ")." : "."),
                                      "Disable /GL for modules you want to hot-reload.", examined));
    } else {
        audit.findings.push_back(make("gl", Verdict::Pass, ad::kWholeProgram, "No compiland of " + module_name + " was built with /GL.", "", examined));
    }

    {
        // The module is what matters for CFG: a CFG-enabled image refuses patched code as a call target. A compiland built with
        // /guard:cf in a module that was linked without /GUARD:CF is a WARN at most (the image has no CFG table).
        const toolchain::FlagFact module_cf = toolchain::detect_module_guard_cf(image);
        const bool image_cfg = module_cf.value == toolchain::Tri::Yes || (module_cf.value == toolchain::Tri::Unknown && lo.guard_cf);
        const std::string evidence = "image: " + module_cf.detail + "; linker /GUARD:CF " + (lo.guard_cf ? "given" : "not seen") + "; " +
                                     std::to_string(s.guard_cf.size()) + " compilands compiled with /guard:cf";
        if (image_cfg) {
            audit.findings.push_back(make("guard-cf", Verdict::Fail, ad::kGuardCf,
                                          "Module " + module_name + " is built with /guard:cf; patched code would not be a valid CFG call target.",
                                          "Build with /guard:cf- for development configurations.", evidence));
        } else if (!s.guard_cf.empty()) {
            audit.findings.push_back(make("guard-cf", Verdict::Warn, ad::kGuardCf,
                                          "Compiland " + s.guard_cf.front() + " of " + module_name + " was compiled with /guard:cf, but the module is not CFG-enabled (not linked with /GUARD:CF).",
                                          "Build with /guard:cf- for development configurations.", evidence));
        } else {
            audit.findings.push_back(make("guard-cf", Verdict::Pass, ad::kGuardCf, "Module " + module_name + " is not built with /guard:cf.", "", evidence));
        }
    }

    if (!s.gy_no.empty()) {
        audit.findings.push_back(make("gy", Verdict::Fail, ad::kNoFunctionLevelLinking,
                                      "Compiland " + s.gy_no.front() + " of " + module_name + " was not built with /Gy; functions cannot be patched individually" +
                                          (s.gy_no.size() > 1 ? " (" + std::to_string(s.gy_no.size()) + " compilands: " + list_names(s.gy_no) + ")." : "."),
                                      "Add /Gy to the compile flags.", examined + "; detection: ADR-0026 (command line, then COMDAT sections of the object)"));
    } else if (!s.gy_unknown.empty()) {
        audit.findings.push_back(make("gy", Verdict::Warn, ad::kNoFunctionLevelLinking,
                                      "Whether " + std::to_string(s.gy_unknown.size()) + " compilands of " + module_name + " were built with /Gy (function-level linking) is unknown (" +
                                          list_names(s.gy_unknown) + "); it is recommended.",
                                      "Add /Gy to the compile flags.", examined + "; neither a /Gy option nor the recorded object could be read"));
    } else {
        audit.findings.push_back(make("gy", Verdict::Pass, ad::kNoFunctionLevelLinking, "Every examined compiland of " + module_name + " was built with /Gy.", "", examined));
    }

    if (!s.managed.empty()) {
        audit.findings.push_back(make("native", Verdict::Fail, ad::kManaged,
                                      "Compiland " + s.managed.front() + " is built with /clr or /ZW; managed and WinRT code cannot be hot-reloaded.",
                                      "Hot-reload only native compilands.", examined));
    } else {
        audit.findings.push_back(make("native", Verdict::Pass, ad::kManaged, "No compiland of " + module_name + " is built with /clr or /ZW.", "", examined));
    }

    // ---- recommended ---------------------------------------------------------------------------------------------------
    {
        const PaddingSample pad = sample_padding(index, image, options);
        const std::string evidence = std::to_string(pad.padded) + " of " + std::to_string(pad.sampled) + " sampled functions have 6 bytes of CC/90 padding before them; " +
                                     std::to_string(s.hotpatch_bit) + " compilands carry the S_COMPILE3 hotpatch bit; " + (lo.known ? lo.detail : "no linker command line recorded");
        if (pad.sampled == 0) {
            audit.findings.push_back(make("hotpatch", Verdict::Warn, ad::kNoHotpatchPadding,
                                          "Could not check whether the functions of " + module_name + " have hot-patch padding (no function could be sampled); /FUNCTIONPADMIN and /hotpatch are recommended.",
                                          "Compile with /hotpatch and link with /FUNCTIONPADMIN.", evidence));
        } else if (pad.padded < pad.sampled) {
            audit.findings.push_back(make("hotpatch", Verdict::Warn, ad::kNoHotpatchPadding,
                                          "Functions of " + module_name + " have no hot-patch padding (" + std::to_string(pad.sampled - pad.padded) + " of " + std::to_string(pad.sampled) +
                                              " sampled); short functions cannot be hooked atomically.",
                                          "Compile with /hotpatch and link with /FUNCTIONPADMIN.", evidence));
        } else {
            audit.findings.push_back(make("hotpatch", Verdict::Pass, ad::kNoHotpatchPadding, "Functions of " + module_name + " have 6 bytes of hot-patch padding.", "", evidence));
        }
    }
    if (!lo.known) {
        audit.findings.push_back(make("link-opt", Verdict::Warn, ad::kOptFoldsOrStrips,
                                      "Could not determine whether " + module_name + " was linked with /OPT:NOREF /OPT:NOICF (the PDB records no linker command line); they are recommended.",
                                      "Link with /OPT:NOREF /OPT:NOICF.", "no linker command line"));
    } else if (lo.opt_ref || lo.opt_icf) {
        audit.findings.push_back(make("link-opt", Verdict::Warn, ad::kOptFoldsOrStrips,
                                      std::string("Module ") + module_name + " is linked with " + (lo.opt_ref ? "/OPT:REF" : "") + (lo.opt_ref && lo.opt_icf ? " and " : "") +
                                          (lo.opt_icf ? "/OPT:ICF" : "") + "; functions may be removed or folded, so a function in the PDB need not have its own body.",
                                      "Link with /OPT:NOREF /OPT:NOICF.", lo.detail));
    } else {
        audit.findings.push_back(make("link-opt", Verdict::Pass, ad::kOptFoldsOrStrips, "Module " + module_name + " is linked with /OPT:NOREF /OPT:NOICF.", "", lo.detail));
    }
    if (!s.inlining.empty()) {
        audit.findings.push_back(make("inlining", Verdict::Warn, ad::kInliningEnabled,
                                      std::to_string(s.inlining.size()) + " of " + std::to_string(s.with_command) + " compilands of " + module_name + " are optimised with inlining (" +
                                          list_names(s.inlining) + "); copies of a changed function inlined elsewhere keep the old code (CML3104).",
                                      "Build iterated modules with /Ob0 or /Od.", examined));
    } else {
        audit.findings.push_back(make("inlining", Verdict::Pass, ad::kInliningEnabled, "No examined compiland of " + module_name + " is optimised with inlining.", "", examined));
    }
}

void audit_image(const std::string& module_path, const pe::Image& image, ModuleAudit& audit) {
    if (image.file_header().machine != 0x8664) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "0x%04X", static_cast<unsigned>(image.file_header().machine));
        audit.findings.push_back(make("image", Verdict::Fail, ad::kNotX64, "Module " + base_of(module_path) + " is machine " + buf + "; only x64 is supported in v1.",
                                      "Use an x64 build.", "PE file header machine"));
        return;
    }
    audit.findings.push_back(make("image", Verdict::Pass, ad::kNotX64, "Module " + base_of(module_path) + " is an x64 image.", "", "PE file header machine 0x8664"));
}

AuditFinding image_failure(const std::string& module_path, const Diag& d) {
    if (d.code == 1303) {  // PE32
        return make("image", Verdict::Fail, ad::kNotX64, "Module " + base_of(module_path) + " is a 32-bit (PE32) image; only x64 is supported in v1.", "Use an x64 build.",
                    "cml_pe: " + d.format());
    }
    return from_diag("image", d);
}

}  // namespace

ModuleAudit audit_opened_module(const std::string& module_path, const pdb::PdbIndex& index, const AuditOptions& options) {
    ModuleAudit audit;
    audit.module_path = module_path;
    audit.pdb_path = index.path();
    Result<pe::OwnedImage> image = pe::load_image_file(module_path);
    if (!image) {
        audit.findings.push_back(image_failure(module_path, image.error()));
        return audit;
    }
    audit_image(module_path, image->image, audit);
    if (audit.has_failures()) {
        return audit;
    }
    audit.findings.push_back(make("pdb", Verdict::Pass, 1410, "The PDB " + base_of(index.path()) + " of " + base_of(module_path) + " was found and matches.", "",
                                  "path " + index.path()));
    audit_with_pdb(module_path, index, image->image, options, audit);
    return audit;
}

ModuleAudit audit_module_file(const std::string& module_path, const pdb::OpenOptions& pdb_options, const AuditOptions& options) {
    ModuleAudit audit;
    audit.module_path = module_path;
    Result<pe::OwnedImage> image = pe::load_image_file(module_path);
    if (!image) {
        audit.findings.push_back(image_failure(module_path, image.error()));
        return audit;
    }
    audit_image(module_path, image->image, audit);
    if (audit.has_failures()) {
        return audit;
    }
    pdb::OpenOptions open_options = pdb_options;
    open_options.use_cache = false;  // ADR-0017: nothing may keep the mapping alive
    Result<std::shared_ptr<const pdb::PdbIndex>> index = pdb::PdbIndex::open_for_pe(module_path, open_options);
    if (!index) {
        audit.findings.push_back(from_diag("pdb", index.error()));
        return audit;
    }
    audit.findings.push_back(make("pdb", Verdict::Pass, 1410, "The PDB " + base_of((*index)->path()) + " of " + base_of(module_path) + " was found and matches.", "",
                                  "path " + (*index)->path()));
    audit_with_pdb(module_path, **index, image->image, options, audit);
    return audit;
}

// ---- reporting --------------------------------------------------------------------------------------------------

std::string format_audit_text(const ModuleAudit& audit, bool verbose) {
    std::string out = "cml_broker check: " + audit.module_path + "\n";
    if (!audit.pdb_path.empty()) {
        out += "  pdb: " + audit.pdb_path + " (" + std::to_string(audit.compilands_total) + " compilands, " + std::to_string(audit.compilands_examined) + " source compilands examined)\n";
    }
    for (const AuditFinding& f : audit.findings) {
        out += std::string("  ") + to_string(f.verdict) + " CML" + std::to_string(f.id) + " " + f.message + "\n";
        if (f.verdict != Verdict::Pass && !f.what_to_do.empty()) {
            out += "       What to do: " + f.what_to_do + "\n";
        }
        if (verbose && !f.evidence.empty()) {
            out += "       evidence: " + f.evidence + "\n";
        }
    }
    out += "result: " + std::to_string(audit.count(Verdict::Fail)) + " FAIL, " + std::to_string(audit.count(Verdict::Warn)) + " WARN, " +
           std::to_string(audit.count(Verdict::Pass)) + " PASS\n";
    return out;
}

ipc::json::Value audit_to_json(const ModuleAudit& audit) {
    using ipc::json::Value;
    Value v = Value::object();
    v.set("module", Value::string(audit.module_path));
    v.set("pdb", Value::string(audit.pdb_path));
    v.set("compilands", Value::integer(static_cast<std::int64_t>(audit.compilands_total)));
    v.set("examined", Value::integer(static_cast<std::int64_t>(audit.compilands_examined)));
    v.set("result", Value::string(audit.has_failures() ? "fail" : audit.count(Verdict::Warn) != 0 ? "warn" : "pass"));
    v.set("fail", Value::integer(static_cast<std::int64_t>(audit.count(Verdict::Fail))));
    v.set("warn", Value::integer(static_cast<std::int64_t>(audit.count(Verdict::Warn))));
    v.set("pass", Value::integer(static_cast<std::int64_t>(audit.count(Verdict::Pass))));
    Value list = Value::array();
    for (const AuditFinding& f : audit.findings) {
        Value item = Value::object();
        item.set("check", Value::string(f.check));
        item.set("verdict", Value::string(to_string(f.verdict)));
        item.set("id", Value::string("CML" + std::to_string(f.id)));
        item.set("message", Value::string(f.message));
        item.set("what_to_do", Value::string(f.what_to_do));
        item.set("evidence", Value::string(f.evidence));
        list.push(std::move(item));
    }
    v.set("findings", std::move(list));
    return v;
}

}  // namespace cml::core
