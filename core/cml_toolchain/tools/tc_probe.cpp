// cml_tc_probe: evidence tool for WP-05 (CI logs are the evidence docs/wp/WP-05.md quotes).
//
//   cml_tc_probe commands <image|pdb> [--obj-dir DIR] [--pch-mode auto|z7|keepzi|nopch]
//       every compiland's recorded build environment, dialect, rewritten command and build-flag facts
//   cml_tc_probe recheck <image|pdb> --obj-dir DIR [--pch-mode M] [--translate-cc1] [--only STEM] [--keep DIR]
//       (Windows) recompile each compiland from its PDB record and compare its symbol set with the
//       original object in DIR. Exit code 1 if any compile fails or any symbol set differs.
//       --translate-cc1 prototypes option (b): clang's cc1 command translated back into a
//       clang-cl driver command line instead of being replayed as `clang -cc1`.
//   cml_tc_probe driver-compile --compiler CLANG-CL --source FILE --out DIR [--include DIR]
//       (Windows) rewrites a hand-made clang-cl DRIVER-form record (what compile_commands.json gives) and
//       compiles it: proves the driver form, including /clang:-gcodeview-command-line, works with that clang
//   cml_tc_probe detect [tool...]    family and version of each tool (default: cl link clang-cl lld-link from PATH)
//   cml_tc_probe flags <image>       module-level /guard:cf fact
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/pdb/pdb.h"
#include "cml/pe/pe.h"
#include "cml/toolchain/toolchain.h"
#include "cml/util/hash.h"
#include "cml/util/log.h"
#include "cml/util/path.h"

namespace tc = cml::toolchain;
using cml::pdb::PdbIndex;

namespace {

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec) && !ec;
}

bool is_pe(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char sig[2] = {0, 0};
    in.read(sig, 2);
    return in.gcount() == 2 && sig[0] == 'M' && sig[1] == 'Z';
}

std::string stem_of(std::string_view object_name) {
    std::string_view base = cml::path::base_name(object_name);
    std::string s(base);
    for (const char* ext : {".cpp.obj", ".cxx.obj", ".cc.obj", ".c.obj", ".obj", ".o"}) {
        if (lower(s).size() >= std::strlen(ext) && lower(s).compare(s.size() - std::strlen(ext), std::strlen(ext), ext) == 0) {
            s.resize(s.size() - std::strlen(ext));
            break;
        }
    }
    return s;
}

tc::PchDebugMode parse_mode(const std::string& s) {
    if (s == "z7") return tc::PchDebugMode::ForceZ7;
    if (s == "keepzi") return tc::PchDebugMode::KeepZiPrivatePdb;
    if (s == "nopch") return tc::PchDebugMode::DropPch;
    return tc::PchDebugMode::Auto;
}

std::string find_object(const std::string& dir, const std::string& stem) {
    for (const char* suffix : {".cpp.obj", ".obj", ".cxx.obj", ".cc.obj", ".c.obj"}) {
        const std::string candidate = dir + "\\" + stem + suffix;
        if (file_exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::vector<std::byte> read_bytes(const std::string& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(s[i]));
    }
    return out;
}

std::shared_ptr<const PdbIndex> open_index(const std::string& input) {
    cml::pdb::OpenOptions opts;
    auto r = is_pe(input) ? PdbIndex::open_for_pe(input, opts) : PdbIndex::open_pdb(input, opts);
    if (!r) {
        std::fprintf(stderr, "%s\n", r.error().format().c_str());
        return nullptr;
    }
    return *r;
}

#if defined(_WIN32)
int cmd_driver_compile(int argc, char** argv) {
    std::string compiler, source, out, include;
    for (int i = 2; i < argc; ++i) {
        const std::string t = argv[i];
        if (t == "--compiler" && i + 1 < argc) compiler = argv[++i];
        else if (t == "--source" && i + 1 < argc) source = argv[++i];
        else if (t == "--out" && i + 1 < argc) out = argv[++i];
        else if (t == "--include" && i + 1 < argc) include = argv[++i];
    }
    if (compiler.empty() || source.empty() || out.empty()) {
        std::fprintf(stderr, "driver-compile needs --compiler, --source and --out\n");
        return 2;
    }
    tc::CompileRequest req;
    req.record.compiler = compiler;
    req.record.source = source;
    req.record.cwd = out;
    req.record.args = {"/nologo", "/TP", "/Zi", "/Od", "/EHsc", "/c", "/FoOLD.obj", "/FdOLD.pdb"};
    if (!include.empty()) req.record.args.push_back("/I" + include);
    req.output_object = tc::patch_object_path(out, "driver", 1, source);
    tc::CompileOutcome o;
    cml::Result<void> r = tc::compile_tu(req, o);
    std::printf("command: %s\n%s", o.command_line.c_str(), o.output.c_str());
    if (!r) {
        std::printf("RESULT driver-compile FAIL %s\n", r.error().format().c_str());
        return 1;
    }
    auto syms = tc::symbol_set_of_file(o.object_path);
    std::printf("RESULT driver-compile OK %zu symbols\n", syms ? syms->size() : 0);
    return syms ? 0 : 1;
}
#endif

struct Args {
    std::string input;
    std::string obj_dir;
    std::string only;
    std::string keep_dir;
    tc::PchDebugMode mode = tc::PchDebugMode::Auto;
    bool translate = false;
};

bool parse_args(int argc, char** argv, int first, Args& a) {
    for (int i = first; i < argc; ++i) {
        const std::string t = argv[i];
        if (t == "--obj-dir" && i + 1 < argc) a.obj_dir = argv[++i];
        else if (t == "--only" && i + 1 < argc) a.only = argv[++i];
        else if (t == "--keep" && i + 1 < argc) a.keep_dir = argv[++i];
        else if (t == "--pch-mode" && i + 1 < argc) a.mode = parse_mode(argv[++i]);
        else if (t == "--translate-cc1") a.translate = true;
        else if (a.input.empty()) a.input = t;
        else return false;
    }
    return !a.input.empty();
}

const char* yn(tc::Tri t) { return tc::to_string(t); }

void print_fact(const char* name, const tc::FlagFact& f) {
    std::printf("    %-9s %-7s [%s] %s\n", name, yn(f.value), tc::to_string(f.source), f.detail.c_str());
}

bool is_source_compiland(const cml::pdb::Compiland& c, const cml::pdb::BuildEnv& env) {
    if (!c.library_name.empty()) {
        return false;
    }
    const auto src = env.source_file();
    return src && !src->empty();
}

#if defined(_WIN32)

// ---- prototype (b): cc1 -> clang-cl driver -----------------------------------------------------
struct Translation {
    std::vector<std::string> args;
    std::vector<std::string> unmapped;
};

Translation translate_cc1_to_driver(const std::vector<std::string>& cc1) {
    Translation t;
    for (std::size_t i = 0; i < cc1.size(); ++i) {
        const std::string& a = cc1[i];
        const auto next = [&]() -> std::string { return i + 1 < cc1.size() ? cc1[++i] : std::string(); };
        if (a == "-cc1" || a == "-emit-obj" || a == "-gcodeview" || a == "-fms-extensions" || a == "-fms-compatibility" ||
            a == "-mrelax-all" || a == "-mincremental-linker-compatible" || a == "-disable-free" ||
            a == "-clear-ast-before-backend" || a == "-disable-llvm-verifier" || a == "-discard-value-names" ||
            a == "-relaxed-aliasing" || a == "-fmath-errno" || a == "-fno-rounding-math" || a == "-mconstructor-aliases" ||
            a == "-fno-use-cxa-atexit" || a == "-fdelayed-template-parsing" || a == "-faddrsig" || a == "-fcolor-diagnostics" ||
            a == "-gno-column-info" || a == "-gcodeview-command-line" || a == "-fexceptions" || a == "-fcxx-exceptions" ||
            a == "-fno-caret-diagnostics") {
            if (a == "-fexceptions" || a == "-fcxx-exceptions") {
                // /EHsc is added once below
            }
            continue;
        }
        if (a == "-triple" || a == "-target-cpu" || a == "-tune-cpu" || a == "-mllvm" || a == "-main-file-name" ||
            a == "-mrelocation-model" || a == "-pic-level" || a == "-mframe-pointer=none" || a == "-resource-dir" ||
            a == "-internal-isystem" || a == "-internal-externc-isystem" || a == "-isystem" || a == "-ferror-limit" ||
            a == "-fmessage-length" || a == "-fdiagnostics-format" || a == "-x" || a == "-funwind-tables=2" ||
            a == "-o" || a == "-include-pch" || a == "-pch-through-header") {
            (void)next();  // host defaults / driver-provided; value dropped
            continue;
        }
        if (a == "-D" || a == "-U" || a == "-I") {
            t.args.push_back("/" + a.substr(1) + next());
        } else if (a.rfind("-std=", 0) == 0) {
            t.args.push_back("/std:" + a.substr(5));
        } else if (a.rfind("--dependent-lib=", 0) == 0) {
            const std::string lib = a.substr(16);
            if (lib == "msvcrt") t.args.push_back("/MD");
            else if (lib == "msvcrtd") t.args.push_back("/MDd");
            else if (lib == "libcmt") t.args.push_back("/MT");
            else if (lib == "libcmtd") t.args.push_back("/MTd");
        } else if (a.rfind("-O", 0) == 0 && a.size() == 3) {
            t.args.push_back(a == "-O0" ? "/Od" : "/" + a.substr(1));
        } else if (a.rfind("-debug-info-kind=", 0) == 0 || a.rfind("-dwarf-version=", 0) == 0 || a.rfind("-fdebug-compilation-dir=", 0) == 0 ||
                   a.rfind("-fms-compatibility-version=", 0) == 0 || a.rfind("-fdeprecated-macro", 0) == 0 ||
                   a.rfind("-fcoverage", 0) == 0 || a.rfind("-ffp-contract=", 0) == 0 || a.rfind("-mrelax", 0) == 0 ||
                   a.rfind("-fno-", 0) == 0 || a.rfind("-f", 0) == 0) {
            // flags the driver also understands (or ignores): pass through with /clang:
            if (a.rfind("-debug-info-kind=", 0) == 0) continue;  // /Z7 below
            t.args.push_back("/clang:" + a);
        } else if (a == "-w" || a.rfind("-W", 0) == 0) {
            t.args.push_back("/clang:" + a);
        } else if (a.empty() || a[0] != '-') {
            t.args.push_back(a);  // the source
        } else {
            t.unmapped.push_back(a);
        }
    }
    t.args.push_back("/EHsc");
    t.args.push_back("/Z7");
    return t;
}

int cmd_recheck(const Args& a) {
    auto idx = open_index(a.input);
    if (!idx) return 1;
    int failures = 0;
    int checked = 0;
    for (std::uint32_t ci = 0; ci < idx->compiland_count(); ++ci) {
        const cml::pdb::Compiland& c = idx->compiland(ci);
        auto env = idx->build_env(ci);
        if (!env || !is_source_compiland(c, *env)) continue;
        const std::string stem = stem_of(c.object_name);
        if (!a.only.empty() && a.only != stem) continue;
        const std::string original = find_object(a.obj_dir, stem);
        if (original.empty()) {
            std::printf("RESULT %s SKIP no original object for the stem in %s\n", stem.c_str(), a.obj_dir.c_str());
            continue;
        }
        auto rec = tc::record_from_build_env(*env);
        if (!rec) {
            std::printf("RESULT %s FAIL %s\n", stem.c_str(), rec.error().format().c_str());
            ++failures;
            continue;
        }
        tc::CompileRequest req;
        req.record = *rec;
        const std::string tmp = std::filesystem::absolute(std::filesystem::path(a.keep_dir.empty() ? std::string("cml_probe_out") : a.keep_dir)).string();
        req.output_object = tc::patch_object_path(tmp, "probe", cml::hash64(c.object_name), rec->source);
        req.pch_debug = a.mode;
        if (a.translate) {
            // Prototype (b): turn the cc1 command into a clang-cl driver command.
            if (tc::detect_dialect(req.record) != tc::Dialect::ClangCc1) {
                std::printf("RESULT %s SKIP --translate-cc1 needs a clang cc1 record\n", stem.c_str());
                continue;
            }
            Translation t = translate_cc1_to_driver(req.record.args);
            std::printf("TRANSLATE %s: %zu driver args, %zu cc1 options with no mapping:", stem.c_str(), t.args.size(), t.unmapped.size());
            for (const std::string& u : t.unmapped) std::printf(" %s", u.c_str());
            std::printf("\n");
            // The recorded compiler is clang.exe or clang-cl.exe; the driver form needs clang-cl.
            std::string compiler = req.record.compiler;
            const std::size_t slash = compiler.find_last_of("\\/");
            req.record.compiler = (slash == std::string::npos ? std::string() : compiler.substr(0, slash + 1)) + "clang-cl.exe";
            req.record.args = t.args;
        }
        tc::CompileOutcome out;
        cml::Result<void> r = tc::compile_tu(req, out);
        std::printf("COMPILE %s: exit=%d %.2fs compiler=%s%s\n", stem.c_str(), out.exit_code, out.seconds, out.compiler_used.c_str(),
                    out.compiler_fallback_used ? " (fallback)" : "");
        std::printf("  command: %s\n", out.command_line.c_str());
        for (const cml::Diag& w : out.warnings) std::printf("  warning: %s\n", w.format().c_str());
        if (!r) {
            if (r.error().code == tc::diag::kPchCreatingTu) {
                std::printf("RESULT %s REJECTED %s\n", stem.c_str(), r.error().format().c_str());
                continue;
            }
            std::printf("%s", out.output.c_str());
            std::printf("RESULT %s FAIL %s\n", stem.c_str(), r.error().format().c_str());
            ++failures;
            continue;
        }
        ++checked;
        auto before = tc::symbol_set_of_file(original);
        auto after = tc::symbol_set_of_file(out.object_path);
        if (!before || !after) {
            std::printf("RESULT %s FAIL cannot read objects: %s\n", stem.c_str(),
                        (!before ? before.error() : after.error()).format().c_str());
            ++failures;
            continue;
        }
        const tc::SymbolSetDiff d = tc::diff_symbol_sets(*before, *after);
        if (d.empty()) {
            std::printf("RESULT %s OK %zu symbols match (defined_external=%zu undefined_external=%zu static=%zu)\n", stem.c_str(),
                        before->size(), before->defined_external.size(), before->undefined_external.size(), before->defined_static.size());
        } else {
            std::printf("RESULT %s MISMATCH\n%s", stem.c_str(), d.to_string().c_str());
            ++failures;
        }
    }
    std::printf("SUMMARY recheck: %d compiled and compared, %d failures\n", checked, failures);
    return failures == 0 ? 0 : 1;
}

#endif

int cmd_commands(const Args& a) {
    auto idx = open_index(a.input);
    if (!idx) return 1;
    int shown = 0;
    for (std::uint32_t ci = 0; ci < idx->compiland_count(); ++ci) {
        const cml::pdb::Compiland& c = idx->compiland(ci);
        auto env = idx->build_env(ci);
        if (!env) {
            std::printf("COMPILAND %u %s: build_env error %s\n", ci, std::string(c.object_name).c_str(), env.error().format().c_str());
            continue;
        }
        if (!is_source_compiland(c, *env)) continue;
        ++shown;
        const std::string stem = stem_of(c.object_name);
        std::printf("COMPILAND %u %s (stem %s)\n", ci, std::string(c.object_name).c_str(), stem.c_str());
        std::printf("  origin=%s\n", cml::pdb::to_string(env->source));
        for (const auto& kv : env->pairs) {
            std::printf("  %s=%s\n", kv.first.c_str(), kv.second.c_str());
        }
        auto rec = tc::record_from_build_env(*env);
        if (!rec) {
            std::printf("  record-error: %s\n", rec.error().format().c_str());
            continue;
        }
        const tc::Dialect dialect = tc::detect_dialect(*rec);
        std::printf("  dialect=%s tokens=%zu\n", tc::to_string(dialect), rec->args.size());
        tc::RewriteOptions ro;
        ro.output_object = tc::patch_object_path("C:\\cml_tmp", "module", cml::hash64(c.object_name), rec->source);
        ro.pch_debug = a.mode;
        auto rw = tc::rewrite_for_patch(*rec, ro);
        if (!rw) {
            std::printf("  rewrite-error: %s\n", rw.error().format().c_str());
        } else {
            std::printf("  rewritten: %s %s\n", rw->compiler.c_str(), rw->command_line.c_str());
            for (const cml::Diag& w : rw->warnings) std::printf("  rewrite-warning: %s\n", w.format().c_str());
            if (!rw->pdb_copy_from.empty()) std::printf("  private-pdb: copy %s -> %s\n", rw->pdb_copy_from.c_str(), rw->pdb_copy_to.c_str());
        }
        tc::CompilandFlagInputs in;
        std::vector<std::string> expanded = rw ? rw->expanded_args : rec->args;
        in.args = expanded;
        in.dialect = dialect;
        in.have_command_line = true;
        in.compile3 = c.has_compile3 ? &c.compile : nullptr;
        std::vector<std::byte> obj;
        if (!a.obj_dir.empty()) {
            const std::string p = find_object(a.obj_dir, stem);
            if (!p.empty()) {
                obj = read_bytes(p);
                in.object_bytes = obj;
            }
        }
        const tc::CompilandFlagFacts f = tc::detect_compiland_flags(in);
        std::printf("  flags (object %s):\n", tc::to_string(f.object_kind));
        print_fact("/GL", f.whole_program_optimization);
        print_fact("/Gy", f.function_level_linking);
        print_fact("/hotpatch", f.hotpatch);
        print_fact("/guard:cf", f.guard_cf);
    }
    std::printf("SUMMARY commands: %d source compilands\n", shown);
    return 0;
}

int cmd_detect(int argc, char** argv, int first) {
    std::vector<std::string> paths;
    for (int i = first; i < argc; ++i) paths.push_back(argv[i]);
    int rc = 0;
    if (paths.empty()) {
        for (const tc::ToolFamily f : {tc::ToolFamily::Cl, tc::ToolFamily::Link, tc::ToolFamily::ClangCl, tc::ToolFamily::LldLink}) {
            auto p = tc::locate_tool(f);
            if (p) paths.push_back(*p);
            else std::printf("%s: %s\n", tc::to_string(f), p.error().format().c_str());
        }
    }
    for (const std::string& p : paths) {
        auto info = tc::detect_tool(p);
        if (!info) {
            std::printf("%s: %s\n", p.c_str(), info.error().format().c_str());
            rc = 1;
            continue;
        }
        std::printf("%s: family=%s version=%s target=%s source=%s\n", p.c_str(), tc::to_string(info->family), info->version_text.c_str(),
                    info->target.c_str(), info->from_version_resource ? "version-resource" : "banner");
    }
    return rc;
}

int cmd_flags(const Args& a) {
    auto img = cml::pe::load_image_file(a.input);
    if (!img) {
        std::fprintf(stderr, "%s\n", img.error().format().c_str());
        return 1;
    }
    const tc::FlagFact f = tc::detect_module_guard_cf(img->image);
    print_fact("/guard:cf", f);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: cml_tc_probe commands|recheck|detect|flags ...\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "detect") {
        return cmd_detect(argc, argv, 2);
    }
#if defined(_WIN32)
    if (cmd == "driver-compile") {
        return cmd_driver_compile(argc, argv);
    }
#endif
    Args a;
    if (!parse_args(argc, argv, 2, a)) {
        std::fprintf(stderr, "bad arguments\n");
        return 2;
    }
    if (cmd == "commands") return cmd_commands(a);
    if (cmd == "flags") return cmd_flags(a);
#if defined(_WIN32)
    if (cmd == "recheck") return cmd_recheck(a);
#endif
    std::fprintf(stderr, "unknown or unsupported command '%s'\n", cmd.c_str());
    return 2;
}
