// The module audit behind `cml_broker check` and the enable-time check (WP-18): every build requirement of plan 4.2 and
// every module-level "unsupported" case of plan section 1, with exact diagnostic ids, on synthetic modules (a PDB from the
// WP-04 builder and a minimal PE), so the expectations do not depend on a compiler. The same checks run on the real
// fixtures through the `check.*` ctest tests (tests/fixtures/bad_builds).
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "../pdb/pdb_builder.h"
#include "../pdb/test_util.h"
#include "cml/core/check_command.h"
#include "cml/core/module_audit.h"
#include "cml/core/module_index.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
namespace tp = cml::testpdb;

namespace {

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

const std::array<std::uint8_t, 16> kGuid = {0x3C, 0x83, 0xC3, 0x8C, 0x00, 0x87, 0x0D, 0xEB, 0x4C, 0x4C, 0x44, 0x20, 0x50, 0x44, 0x42, 0x2E};

constexpr std::uint32_t kFlagLtcg = 1u << 10;
constexpr std::uint32_t kFlagManaged = 1u << 12;

const char* const kGoodCompile = "/c /Zi /Gy /hotpatch /Od a.cpp";
const char* const kGoodLink = "/DEBUG:FULL /INCREMENTAL:NO /OPT:NOREF /OPT:NOICF /FUNCTIONPADMIN /OUT:app.exe";

struct ModOpts {
    std::string compile = kGoodCompile;      // recorded compile command; "none" = no command line recorded
    std::string link = kGoodLink;            // recorded link command; "" = the linker module has no S_ENVBLOCK
    std::uint32_t compile3_flags = 0;
    bool procs = true;                       // the TU lists its function (false: a FASTLINK-like PDB)
    std::uint16_t pe_machine = 0x8664;
    bool write_pdb = true;
    std::array<std::uint8_t, 16> pdb_guid = kGuid;
    bool second_tu_without_command = false;
    std::string link_exe = "C:\\vc\\bin\\link.exe";  // what the linker module records as its program
    bool no_contributions = false;            // no section contributions: no function symbol can be sampled
    bool cfg_image = false;                   // DllCharacteristics GUARD_CF set in the PE
};

// One synthetic module on disk: app.exe + app.pdb with one translation unit "a.cpp.obj" and a `* Linker *` compiland.
struct Module {
    tp::TempDir dir;
    tp::Spec spec;
    std::string exe;
    std::string pdb;


    explicit Module(const ModOpts& o = ModOpts()) {
        spec.guid = kGuid;
        spec.age = 1;
        spec.sections = {{".text", 0x1000, 0x1000, 0x1000, 0x60000020}, {".data", 0x800, 0x2000, 0x800, 0xC0000040}};
        add_tu("C:\\proj\\obj\\a.cpp.obj", "C:\\proj\\a.cpp", o.compile, o.compile3_flags, o.procs, 0x10);
        if (o.second_tu_without_command) {
            add_tu("C:\\proj\\obj\\b.cpp.obj", "C:\\proj\\b.cpp", "none", 0, true, 0x110);
        }
        tp::Module linker;
        linker.name = "* Linker *";
        if (!o.link.empty()) {
            linker.symbols.envblock({{"cwd", "C:\\proj"}, {"exe", o.link_exe}, {"cmd", o.link}});
        }
        spec.modules.push_back(std::move(linker));
        const std::uint16_t last = static_cast<std::uint16_t>(spec.modules.size() - 1);
        if (!o.no_contributions) {
            spec.contributions = {{1, 0x000, 0x100, 0}, {1, 0x100, 0x100, o.second_tu_without_command ? std::uint16_t(1) : last}};
        }
        spec.symrec.pub32("a_func", 0x3, 1, 0x10);
        pdb = dir.file("app.pdb");
        exe = dir.file("app.exe");
        if (o.write_pdb) {
            tp::Spec written = spec;
            written.guid = o.pdb_guid;
            tp::write_file(pdb, tp::build(written));
        }
        tp::Bytes pe = tp::build_pe(kGuid, 1, pdb);
        pe[0x84] = static_cast<std::uint8_t>(o.pe_machine & 0xFF);
        pe[0x85] = static_cast<std::uint8_t>(o.pe_machine >> 8);
        if (o.cfg_image) {
            pe[0xDF] = static_cast<std::uint8_t>(pe[0xDF] | 0x40);  // DllCharacteristics (optional header + 70) |= IMAGE_DLLCHARACTERISTICS_GUARD_CF
        }
        tp::write_file(exe, pe);
    }

    void add_tu(const std::string& obj, const std::string& src, const std::string& command, std::uint32_t flags, bool procs, std::uint32_t text_offset) {
        tp::Module tu;
        tu.name = obj;
        tu.symbols.objname(obj);
        tu.symbols.compile3(1, flags, 0xD0, "Test Compiler 19.44");
        if (command == "none") {
            tu.symbols.envblock({{"cwd", "C:\\proj"}, {"src", src}});
        } else {
            tu.symbols.envblock({{"cwd", "C:\\proj"}, {"cl", "C:\\vc\\bin\\cl.exe"}, {"cmd", command}, {"src", src}});
        }
        if (procs) {
            tu.symbols.proc(0x1110, "a_func", 1, text_offset, 0x20);
            tu.symbols.end();
        }
        spec.modules.push_back(std::move(tu));
    }
};

const AuditFinding& finding(const ModuleAudit& a, const char* check) {
    const AuditFinding* f = a.find(check);
    REQUIRE_MESSAGE(f != nullptr, "no finding " << check);
    return *f;
}

std::FILE* open_scratch(const std::string& path) {
#if defined(_WIN32)
    std::FILE* f = nullptr;
    return fopen_s(&f, path.c_str(), "w+b") == 0 ? f : nullptr;
#else
    return std::fopen(path.c_str(), "w+b");
#endif
}

std::string capture(int* code, const std::vector<std::string>& args, std::string* err_text = nullptr) {
    // (not std::tmpfile(): on Windows it writes to the root of the drive, which needs administrator rights)
    tp::TempDir scratch;
    std::FILE* out = open_scratch(scratch.file("out.txt"));
    std::FILE* err = open_scratch(scratch.file("err.txt"));
    REQUIRE(out != nullptr);
    REQUIRE(err != nullptr);
    *code = run_check_command(args, out, err);
    auto slurp = [](std::FILE* f) {
        std::string text;
        std::rewind(f);
        char buf[512];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
            text.append(buf, n);
        }
        std::fclose(f);
        return text;
    };
    if (err_text != nullptr) {
        *err_text = slurp(err);
    } else {
        std::fclose(err);
    }
    return slurp(out);
}

}  // namespace

TEST_CASE("audit: a module that meets every requirement passes every check") {
    QuietLogs quiet;
    Module m;
    const ModuleAudit a = audit_module_file(m.exe);
    CHECK_FALSE(a.has_failures());
    for (const char* check : {"image", "pdb", "debug-info", "compile-command", "gl", "guard-cf", "gy", "native", "link-opt", "inlining"}) {
        CHECK_MESSAGE(finding(a, check).verdict == Verdict::Pass, check << ": " << finding(a, check).message);
    }
    // The fake PE has no code behind the PDB's RVAs, so the padding check cannot sample: a WARN, never a FAIL.
    CHECK(finding(a, "hotpatch").verdict == Verdict::Warn);
    CHECK(finding(a, "hotpatch").id == 1601);
    CHECK(a.compilands_examined == 1);
}

TEST_CASE("audit: /GL is CML1103 FAIL, from the command line and from the S_COMPILE3 LTCG bit with it") {
    QuietLogs quiet;
    ModOpts o;
    o.compile = "/c /Zi /Gy /GL a.cpp";
    Module m(o);
    const ModuleAudit a = audit_module_file(m.exe);
    CHECK(a.has_failures());
    CHECK(finding(a, "gl").verdict == Verdict::Fail);
    CHECK(finding(a, "gl").id == 1103);
    CHECK(a.first_failure()->id == 1103);
    CHECK(finding(a, "gl").message.find("a.cpp.obj") != std::string::npos);
    CHECK(finding(a, "gl").message.find("/GL") != std::string::npos);
    CHECK_FALSE(finding(a, "gl").what_to_do.empty());

    // The compiler's own record (S_COMPILE3 LTCG bit) is enough, whatever the recorded command says.
    ModOpts bit;
    bit.compile3_flags = kFlagLtcg;
    Module m2(bit);
    const ModuleAudit b = audit_module_file(m2.exe);
    CHECK(finding(b, "gl").verdict == Verdict::Fail);
    CHECK(finding(b, "gl").id == 1103);
}

TEST_CASE("audit: /guard:cf is CML1104 FAIL when the image is CFG-enabled, WARN when only a compiland was compiled with it") {
    QuietLogs quiet;
    ModOpts o;
    o.compile = "/c /Zi /Gy /guard:cf a.cpp";
    o.cfg_image = true;
    Module m(o);
    const ModuleAudit a = audit_module_file(m.exe);
    CHECK(finding(a, "guard-cf").verdict == Verdict::Fail);
    CHECK(finding(a, "guard-cf").id == 1104);
    CHECK(a.has_failures());

    // Compiled with /guard:cf but linked without /GUARD:CF: the image has no CFG table, so only a warning.
    ModOpts compile_only;
    compile_only.compile = "/c /Zi /Gy /guard:cf a.cpp";
    Module m1(compile_only);
    const ModuleAudit w = audit_module_file(m1.exe);
    CHECK(finding(w, "guard-cf").verdict == Verdict::Warn);
    CHECK(finding(w, "guard-cf").id == 1104);
    CHECK_FALSE(w.has_failures());

    // clang's spelling in a cc1 command
    ModOpts c;
    c.compile = "-cc1 -triple x86_64-pc-windows-msvc19.40.0 -ffunction-sections -cfguard a.cpp";
    Module m2(c);
    CHECK(finding(audit_module_file(m2.exe), "guard-cf").verdict == Verdict::Warn);
    c.cfg_image = true;
    Module m3(c);
    CHECK(finding(audit_module_file(m3.exe), "guard-cf").verdict == Verdict::Fail);

    // Image flag unknown (a function table without the DllCharacteristics bit cannot be told apart): the link option decides.
    CHECK(parse_link_options("/DEBUG:FULL /GUARD:CF").guard_cf);
}

TEST_CASE("audit: /Gy absent is CML1105 FAIL; /ZI and /O2 imply it") {
    QuietLogs quiet;
    ModOpts o;
    o.compile = "/c /Zi /Od a.cpp";
    Module m(o);
    const ModuleAudit a = audit_module_file(m.exe);
    CHECK(finding(a, "gy").verdict == Verdict::Fail);
    CHECK(finding(a, "gy").id == 1105);
    ModOpts implied;
    implied.compile = "/c /ZI /Od a.cpp";
    Module m2(implied);
    CHECK(finding(audit_module_file(m2.exe), "gy").verdict == Verdict::Pass);
}

TEST_CASE("audit: a compiland without a recorded command line is CML1102 (FAIL when none has one, WARN when some do)") {
    QuietLogs quiet;
    ModOpts o;
    o.compile = "none";
    Module none(o);
    const ModuleAudit a = audit_module_file(none.exe);
    CHECK(finding(a, "compile-command").verdict == Verdict::Fail);
    CHECK(finding(a, "compile-command").id == 1102);
    CHECK(a.has_failures());

    ModOpts mixed;
    mixed.second_tu_without_command = true;
    Module m(mixed);
    const ModuleAudit b = audit_module_file(m.exe);
    CHECK(finding(b, "compile-command").verdict == Verdict::Warn);
    CHECK(finding(b, "compile-command").id == 1102);
    CHECK(finding(b, "compile-command").message.find("b.cpp.obj") != std::string::npos);
    CHECK_FALSE(b.has_failures());
}

TEST_CASE("audit: a PDB without function symbols is CML1101 FAIL; the command line only decides when the PDB cannot say") {
    QuietLogs quiet;
    // The PDB is the evidence: compilands that contributed code but list no function are a FASTLINK / stripped PDB.
    ModOpts bare;
    bare.link = "";
    bare.procs = false;
    Module m(bare);
    const ModuleAudit b = audit_module_file(m.exe);
    CHECK(finding(b, "debug-info").verdict == Verdict::Fail);
    CHECK(finding(b, "debug-info").id == 1101);
    CHECK(b.has_failures());

    // /DEBUG:FASTLINK on the command line, but the PDB has its function symbols: a full PDB, no FAIL.
    ModOpts overridden;
    overridden.link = "/DEBUG:FASTLINK /DEBUG:FULL /INCREMENTAL:NO /OUT:app.exe";
    Module m2(overridden);
    CHECK(finding(audit_module_file(m2.exe), "debug-info").verdict == Verdict::Pass);
    ModOpts contradicting;
    contradicting.link = "/DEBUG:FASTLINK /INCREMENTAL:NO /OUT:app.exe";
    Module m3(contradicting);
    CHECK(finding(audit_module_file(m3.exe), "debug-info").verdict == Verdict::Pass);  // the symbols are there

    // Nothing to sample (no section contributions): the command line decides, for link.exe ...
    ModOpts unsampled;
    unsampled.no_contributions = true;
    unsampled.link = "/DEBUG:FASTLINK /OUT:app.exe";
    Module m4(unsampled);
    const ModuleAudit f = audit_module_file(m4.exe);
    CHECK(finding(f, "debug-info").verdict == Verdict::Fail);
    CHECK(finding(f, "debug-info").id == 1101);
    CHECK(finding(f, "debug-info").message.find("FASTLINK") != std::string::npos);
    // ... a later /DEBUG:FULL wins ...
    unsampled.link = "/DEBUG:FASTLINK /DEBUG:FULL /OUT:app.exe";
    Module m5(unsampled);
    CHECK(finding(audit_module_file(m5.exe), "debug-info").verdict == Verdict::Pass);
    // ... and lld-link writes a full PDB whatever it is given.
    unsampled.link = "/DEBUG:FASTLINK /OUT:app.exe";
    unsampled.link_exe = "C:\\Program Files\\LLVM\\bin\\lld-link.exe";
    Module m6(unsampled);
    CHECK(finding(audit_module_file(m6.exe), "debug-info").verdict == Verdict::Pass);
}

TEST_CASE("audit: a missing PDB is CML1410 FAIL, a PDB of another build CML1411 FAIL") {
    QuietLogs quiet;
    ModOpts o;
    o.write_pdb = false;
    Module m(o);
    const ModuleAudit a = audit_module_file(m.exe);
    REQUIRE(a.find("pdb") != nullptr);
    CHECK(finding(a, "pdb").verdict == Verdict::Fail);
    CHECK(finding(a, "pdb").id == 1410);
    CHECK(finding(a, "image").verdict == Verdict::Pass);
    CHECK(a.find("gl") == nullptr);  // the remaining checks need the PDB

    ModOpts other;
    other.pdb_guid = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    Module m2(other);
    const ModuleAudit b = audit_module_file(m2.exe);
    CHECK(finding(b, "pdb").verdict == Verdict::Fail);
    CHECK(finding(b, "pdb").id == 1411);
}

TEST_CASE("audit: a module that is not x64 is CML1106 FAIL; a file that is not a PE fails with the reader's id") {
    QuietLogs quiet;
    ModOpts o;
    o.pe_machine = 0x014C;
    Module m(o);
    const ModuleAudit a = audit_module_file(m.exe);
    CHECK(finding(a, "image").verdict == Verdict::Fail);
    CHECK(finding(a, "image").id == 1106);
    CHECK(a.find("pdb") == nullptr);
    o.pe_machine = 0xAA64;
    Module arm(o);
    CHECK(finding(audit_module_file(arm.exe), "image").id == 1106);

    tp::TempDir dir;
    tp::write_file(dir.file("junk.exe"), tp::Bytes(100, 7));
    const ModuleAudit junk = audit_module_file(dir.file("junk.exe"));
    CHECK(junk.has_failures());
    CHECK(junk.first_failure()->id >= 1300);
    CHECK(junk.first_failure()->id < 1400);
    const ModuleAudit missing = audit_module_file(dir.file("nothing.exe"));
    CHECK(missing.first_failure()->id == 1300);
}

TEST_CASE("audit: /clr compilands are CML1504 FAIL") {
    QuietLogs quiet;
    ModOpts o;
    o.compile3_flags = kFlagManaged;
    Module m(o);
    const ModuleAudit a = audit_module_file(m.exe);
    CHECK(finding(a, "native").verdict == Verdict::Fail);
    CHECK(finding(a, "native").id == 1504);
}

TEST_CASE("audit: recommended flags warn (CML1602 /OPT, CML1603 inlining) and never fail") {
    QuietLogs quiet;
    ModOpts o;
    o.compile = "/c /Zi /Gy /O2 a.cpp";
    o.link = "/DEBUG:FULL /OPT:REF /OPT:ICF /OUT:app.exe";
    Module m(o);
    const ModuleAudit a = audit_module_file(m.exe);
    CHECK_FALSE(a.has_failures());
    CHECK(finding(a, "link-opt").verdict == Verdict::Warn);
    CHECK(finding(a, "link-opt").id == 1602);
    CHECK(finding(a, "inlining").verdict == Verdict::Warn);
    CHECK(finding(a, "inlining").id == 1603);

    ModOpts unknown;
    unknown.link = "";
    Module m2(unknown);
    const ModuleAudit b = audit_module_file(m2.exe);
    CHECK(finding(b, "link-opt").verdict == Verdict::Warn);  // "unknown, recommended"
    CHECK_FALSE(b.has_failures());

    ModOpts ob0;
    ob0.compile = "/c /Zi /Gy /O2 /Ob0 a.cpp";
    Module m3(ob0);
    CHECK(finding(audit_module_file(m3.exe), "inlining").verdict == Verdict::Pass);
}

TEST_CASE("link options: defaults follow /DEBUG, explicit options win, the last one wins") {
    LinkOptionsFacts none = parse_link_options("");
    CHECK_FALSE(none.known);
    LinkOptionsFacts debug = parse_link_options("/DEBUG:FULL /OUT:a.exe");
    CHECK(debug.known);
    CHECK(debug.debug_fastlink.has_value());
    CHECK_FALSE(*debug.debug_fastlink);
    CHECK_FALSE(debug.opt_ref);  // /DEBUG makes NOREF, NOICF the default
    CHECK_FALSE(debug.opt_icf);
    LinkOptionsFacts release = parse_link_options("/OUT:a.exe /INCREMENTAL:NO");
    CHECK(release.opt_ref);  // no /DEBUG: REF and ICF are the default
    CHECK(release.opt_icf);
    LinkOptionsFacts both = parse_link_options("/DEBUG /opt:ref,icf=3 /opt:noicf /functionpadmin:6");
    CHECK(both.opt_ref);
    CHECK_FALSE(both.opt_icf);
    CHECK(both.function_pad_min);
    LinkOptionsFacts fast = parse_link_options("-debug:fastlink -GUARD:CF");
    CHECK(*fast.debug_fastlink);
    CHECK(fast.guard_cf);
    // /DEBUG is last-wins (link.exe).
    CHECK_FALSE(*parse_link_options("/DEBUG:FASTLINK /DEBUG:FULL").debug_fastlink);
    CHECK(*parse_link_options("/DEBUG:FULL /DEBUG:FASTLINK").debug_fastlink);
    CHECK_FALSE(*parse_link_options("/DEBUG:FASTLINK /DEBUG").debug_fastlink);
    CHECK_FALSE(parse_link_options("/DEBUG:FASTLINK /DEBUG:NONE").debug_fastlink.has_value());
}

TEST_CASE("inlining: only optimised compilands without /Ob0 or -fno-inline allow it") {
    CHECK(command_allows_inlining("/c /Zi /O2 a.cpp"));
    CHECK(command_allows_inlining("/c /Zi /O2 /Ob2 a.cpp"));
    CHECK(command_allows_inlining("-cc1 -triple x86_64-pc-windows-msvc -O2 a.cpp"));
    CHECK_FALSE(command_allows_inlining("/c /Zi /O2 /Ob0 a.cpp"));
    CHECK_FALSE(command_allows_inlining("/c /Zi /Od a.cpp"));
    CHECK_FALSE(command_allows_inlining("/c /Zi a.cpp"));
    CHECK_FALSE(command_allows_inlining("-cc1 -O2 -fno-inline a.cpp"));
    CHECK_FALSE(command_allows_inlining("-cc1 -O0 a.cpp"));
    CHECK_FALSE(command_allows_inlining(""));
}

TEST_CASE("CML3104: a changed small function of an optimised compiland warns; big ones and /Od ones do not") {
    QuietLogs quiet;
    ModOpts o;
    o.compile = "/c /Zi /Gy /O2 a.cpp";
    Module m(o);
    pdb::OpenOptions po;
    po.use_cache = false;
    auto index = pdb::PdbIndex::open_for_pe(m.exe, po);
    REQUIRE_MESSAGE(index.has_value(), index.error().format());
    const std::uint32_t ci = (*index)->compilands_by_object_stem("a")[0];
    const std::optional<Diag> small = stale_inline_warning(**index, ci, {"a_func", "b_func"}, "app.exe");
    REQUIRE(small.has_value());
    CHECK(small->code == 3104);
    CHECK(small->message.find("a_func") != std::string::npos);
    CHECK(small->message.find("b_func") != std::string::npos);
    CHECK(small->message.find("a.cpp.obj") != std::string::npos);
    CHECK(small->message.find("2 small changed function") != std::string::npos);  // one diagnostic for the whole compiland
    CHECK(small->message.find("other translation unit") == std::string::npos);    // that is not what happens (docs/wp/WP-18.md)
    CHECK_FALSE(small->what_to_do.empty());
    CHECK_FALSE(stale_inline_warning(**index, ci, {}, "app.exe").has_value());

    ModOpts od;
    Module m2(od);
    auto index2 = pdb::PdbIndex::open_for_pe(m2.exe, po);
    REQUIRE(index2.has_value());
    CHECK_FALSE(stale_inline_warning(**index2, (*index2)->compilands_by_object_stem("a")[0], {"a_func"}, "app.exe").has_value());
}

TEST_CASE("check command: exit code 0 without FAIL, 1 with, 2 for a bad command line; text and JSON output") {
    QuietLogs quiet;
    Module good;
    int code = -1;
    std::string text = capture(&code, {good.exe});
    CHECK(code == 0);
    CHECK(text.find("PASS CML1103") != std::string::npos);
    CHECK(text.find("result: 0 FAIL") != std::string::npos);

    ModOpts o;
    o.compile = "/c /Zi /Gy /GL a.cpp";
    Module bad(o);
    text = capture(&code, {bad.exe});
    CHECK(code == 1);
    CHECK(text.find("FAIL CML1103") != std::string::npos);
    CHECK(text.find("What to do:") != std::string::npos);
    CHECK(text.find("evidence:") == std::string::npos);
    text = capture(&code, {"--verbose", bad.exe});
    CHECK(text.find("evidence:") != std::string::npos);

    text = capture(&code, {"--json", bad.exe});
    CHECK(code == 1);
    auto parsed = ipc::json::parse(text);
    REQUIRE_MESSAGE(parsed.has_value(), text);
    CHECK(parsed->get_string("result") == "fail");
    bool seen = false;
    for (const auto& item : parsed->find("findings")->items()) {
        if (item.get_string("id") == "CML1103") {
            seen = true;
            CHECK(item.get_string("verdict") == "FAIL");
        }
    }
    CHECK(seen);

    std::string err;
    capture(&code, {}, &err);
    CHECK(code == 2);
    CHECK(err.find("no module given") != std::string::npos);
    capture(&code, {"--bogus", good.exe}, &err);
    CHECK(code == 2);
    capture(&code, {good.exe, bad.exe}, &err);
    CHECK(code == 2);
    capture(&code, {"--pdb-dir"}, &err);
    CHECK(code == 2);
    capture(&code, {"--help"}, &err);
    CHECK(code == 0);

    // The module cannot be read at all: a FAIL, exit code 1 (not a usage error).
    capture(&code, {good.dir.file("nothing.exe")});
    CHECK(code == 1);
}

TEST_CASE("enable time: the index of a module that fails the audit fails with that finding; one that passes is ready") {
    QuietLogs quiet;
    ModOpts o;
    o.compile = "/c /Zi /Gy /guard:cf a.cpp";
    o.cfg_image = true;
    Module bad(o);
    std::atomic<int> audits{0};
    std::string seen;
    ModuleIndexSpec spec;
    spec.module_path = bad.exe;
    spec.audit = true;
    std::shared_ptr<ModuleIndex> index_for_hook;
    std::mutex hook_mutex;
    spec.on_audit = [&](const ModuleAudit& a) {
        ++audits;
        // Between the audit and the Failed state: the PDB is still available (and is handed out), but it must not stay mapped afterwards.
        std::lock_guard<std::mutex> lock(hook_mutex);
        if (index_for_hook) {
            (void)index_for_hook->acquire_pdb();
        }
        seen = a.first_failure() != nullptr ? a.first_failure()->diag().id() : "";
    };
    auto index = ModuleIndex::start(spec);
    {
        std::lock_guard<std::mutex> lock(hook_mutex);
        index_for_hook = index;
    }
    REQUIRE(index->wait(20000));
    CHECK(index->status().state == IndexState::Failed);
    REQUIRE(index->status().error.has_value());
    CHECK(index->status().error->code == 1104);
    CHECK(audits.load() == 1);
    CHECK(seen == "CML1104");
    CHECK(index->audit().has_value());
    CHECK(index->pdb() == nullptr);
    {
        std::lock_guard<std::mutex> lock(hook_mutex);
        index_for_hook.reset();  // (the hook holds the index: break the cycle before it is destroyed)
    }  // released: a module that cannot be enabled keeps no mapping (ADR-0017)
    CHECK_FALSE(index->acquire_pdb().has_value());

    Module good;
    ModuleIndexSpec ok;
    ok.module_path = good.exe;
    ok.audit = true;
    auto index2 = ModuleIndex::start(ok);
    REQUIRE(index2->wait(20000));
    CHECK(index2->status().state == IndexState::Ready);
    REQUIRE(index2->audit().has_value());
    CHECK_FALSE(index2->audit()->has_failures());

    // Without spec.audit nothing is checked (the synthetic modules of the other tests need that).
    ModuleIndexSpec off;
    off.module_path = bad.exe;
    auto index3 = ModuleIndex::start(off);
    REQUIRE(index3->wait(20000));
    CHECK(index3->status().state == IndexState::Ready);
    CHECK_FALSE(index3->audit().has_value());
}
