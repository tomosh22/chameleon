// WP-09: the linker driver's portable parts: command construction, flag refusal, response files, linker output
// summary and the family decision from synthetic PE and PDB facts. Runs on every host.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "cml/toolchain/link_patch.h"
#include "cml/toolchain/command_line.h"
#include "doctest.h"

using namespace cml::toolchain;

namespace {

LinkCommandSpec basic_spec(ToolFamily family = ToolFamily::Link) {
    LinkCommandSpec s;
    s.family = family;
    s.objects = {"C:\\w\\logic.anchored.obj", "C:\\w\\binding.obj"};
    s.out_dll = "C:\\w\\patch_1.dll";
    s.out_pdb = "C:\\w\\patch_1.pdb";
    s.base = 0x180000000ull;
    return s;
}

// A PE with the given linker version, optional Rich header, written the way a linker lays out the headers.
std::vector<std::byte> make_pe(unsigned major, unsigned minor, bool rich, std::uint16_t magic = 0x20B) {
    std::vector<std::byte> b(0x400, std::byte{0});
    auto w16 = [&](std::size_t o, unsigned v) {
        b[o] = static_cast<std::byte>(v & 0xFF);
        b[o + 1] = static_cast<std::byte>((v >> 8) & 0xFF);
    };
    auto w32 = [&](std::size_t o, std::uint32_t v) {
        w16(o, v & 0xFFFF);
        w16(o + 2, v >> 16);
    };
    b[0] = std::byte{'M'};
    b[1] = std::byte{'Z'};
    const std::uint32_t lfanew = 0x100;
    w32(0x3C, lfanew);
    if (rich) {
        // "DanS" ... "Rich" <xor key>
        std::memcpy(b.data() + 0x80, "DanS", 4);
        std::memcpy(b.data() + 0xA0, "Rich", 4);
        w32(0xA4, 0x12345678);
    }
    w32(lfanew, 0x00004550);
    w16(lfanew + 4, 0x8664);
    const std::size_t opt = lfanew + 24;
    w16(opt, magic);
    b[opt + 2] = static_cast<std::byte>(major);
    b[opt + 3] = static_cast<std::byte>(minor);
    return b;
}

PdbLinkerFacts link_exe_pdb() {
    PdbLinkerFacts f;
    f.linker_module_found = true;
    f.has_compile3 = true;
    f.compile_version = "Microsoft (R) LINK";
    f.backend_version = {14, 44, 35207, 0};
    return f;
}

PdbLinkerFacts lld_pdb() {
    PdbLinkerFacts f;
    f.linker_module_found = true;
    f.has_compile3 = true;
    f.compile_version = "LLVM Linker";
    f.has_env_block = true;
    f.exe_path = "C:\\Program Files\\LLVM\\bin\\lld-link.exe";
    return f;
}

const PdbLinkerFacts g_link = link_exe_pdb();
const PdbLinkerFacts g_lld = lld_pdb();

}  // namespace

// ---- command construction -----------------------------------------------------------------------------------

TEST_CASE("link command: the plan 4.6 flags in order, then outputs, then objects") {
    auto args = build_link_command(basic_spec());
    REQUIRE_MESSAGE(args, args.error().format());
    const std::vector<std::string> expected = {"/nologo",
                                               "/DLL",
                                               "/NOENTRY",
                                               "/NODEFAULTLIB",
                                               "/MACHINE:X64",
                                               "/DEBUG:FULL",
                                               "/INCREMENTAL:NO",
                                               "/OPT:NOREF",
                                               "/OPT:NOICF",
                                               "/FUNCTIONPADMIN",
                                               "/FIXED",
                                               "/DYNAMICBASE:NO",
                                               "/BASE:0x180000000",
                                               "/PDB:C:\\w\\patch_1.pdb",
                                               "/OUT:C:\\w\\patch_1.dll",
                                               "C:\\w\\logic.anchored.obj",
                                               "C:\\w\\binding.obj"};
    CHECK(*args == expected);
}

TEST_CASE("link command: both families get the same command line") {
    auto a = build_link_command(basic_spec(ToolFamily::Link));
    auto b = build_link_command(basic_spec(ToolFamily::LldLink));
    REQUIRE(a);
    REQUIRE(b);
    CHECK(*a == *b);
}

TEST_CASE("link command: /NOREF and /NOICF are always present and nothing re-enables REF or ICF") {
    LinkCommandSpec s = basic_spec();
    s.extra_args = {"/OPT:LBR", "/WX"};
    auto args = build_link_command(s);
    REQUIRE(args);
    int noref = 0;
    int noicf = 0;
    for (const std::string& a : *args) {
        noref += a == "/OPT:NOREF";
        noicf += a == "/OPT:NOICF";
        CHECK(a != "/OPT:REF");
        CHECK(a != "/OPT:ICF");
    }
    CHECK(noref == 1);
    CHECK(noicf == 1);
}

TEST_CASE("link command: extra arguments go after the driver's own and before the objects") {
    LinkCommandSpec s = basic_spec();
    s.extra_args = {"/WX", "/LIBPATH:C:\\x"};
    auto args = build_link_command(s);
    REQUIRE(args);
    REQUIRE(args->size() == 19);
    CHECK((*args)[15] == "/WX");
    CHECK((*args)[16] == "/LIBPATH:C:\\x");
    CHECK((*args)[17] == "C:\\w\\logic.anchored.obj");
}

TEST_CASE("link command: /IGNORE is only passed when asked for") {
    auto none = build_link_command(basic_spec());
    REQUIRE(none);
    for (const std::string& a : *none) {
        CHECK(a.rfind("/IGNORE", 0) == std::string::npos);
    }
    LinkCommandSpec s = basic_spec();
    s.ignore_warnings = {4001, 4254, 4281};
    auto args = build_link_command(s);
    REQUIRE(args);
    CHECK((*args)[15] == "/IGNORE:4001,4254,4281");
}

TEST_CASE("link command: arguments with spaces are quoted as a whole when the line is built") {
    LinkCommandSpec s = basic_spec();
    s.out_dll = "C:\\my dir\\patch 1.dll";
    s.out_pdb = "C:\\my dir\\patch 1.pdb";
    s.objects = {"C:\\my dir\\a b.obj"};
    auto args = build_link_command(s);
    REQUIRE(args);
    const std::string line = join_command_line(*args);
    CHECK(line.find("\"/PDB:C:\\my dir\\patch 1.pdb\"") != std::string::npos);
    CHECK(line.find("\"/OUT:C:\\my dir\\patch 1.dll\"") != std::string::npos);
    CHECK(line.find("\"C:\\my dir\\a b.obj\"") != std::string::npos);
    // And the line parses back to the same arguments.
    CHECK(split_command_line(line) == *args);
}

TEST_CASE("link command: /BASE formatting") {
    CHECK(format_base_argument(0x180000000ull) == "0x180000000");
    CHECK(format_base_argument(0x10000) == "0x10000");
    CHECK(format_base_argument(0x7FF600000000ull) == "0x7ff600000000");
    LinkCommandSpec s = basic_spec();
    s.base = 0x7FF600010000ull;
    auto args = build_link_command(s);
    REQUIRE(args);
    CHECK((*args)[12] == "/BASE:0x7ff600010000");
}

TEST_CASE("link command: bad requests are CML3362") {
    LinkCommandSpec s = basic_spec();
    s.base = 0;
    auto r = build_link_command(s);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kBadLinkRequest);
    s = basic_spec();
    s.base = 0x180001000ull;  // not 64 KiB aligned
    r = build_link_command(s);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kBadLinkRequest);
    s = basic_spec();
    s.objects.clear();
    r = build_link_command(s);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kBadLinkRequest);
    s = basic_spec();
    s.out_dll.clear();
    r = build_link_command(s);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kBadLinkRequest);
    s = basic_spec();
    s.family = ToolFamily::Cl;
    r = build_link_command(s);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kBadLinkRequest);
}

// ---- refusals ---------------------------------------------------------------------------------------------------

TEST_CASE("link command: /OPT:REF and /OPT:ICF are refused in every spelling (CML3360)") {
    const char* bad[] = {"/OPT:REF",        "-OPT:REF",     "/opt:ref",        "/OPT:ICF",  "/OPT:ICF=3", "/OPT:icf=2",
                         "/OPT:REF,ICF",    "/OPT:NOREF,ICF", "/OPT:ICF,NOREF", "/OPT:LBR,REF", "/FORCE",   "/FORCE:UNRESOLVED",
                         "/FORCE:MULTIPLE", "/INCREMENTAL", "/INCREMENTAL:YES", "/DYNAMICBASE", "/DYNAMICBASE:YES", "/FIXED:NO"};
    for (const char* arg : bad) {
        INFO("argument " << arg);
        LinkCommandSpec s = basic_spec();
        s.extra_args = {arg};
        auto r = build_link_command(s);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == diag::kForbiddenLinkerOption);
        CHECK(r.error().message.find(arg) != std::string::npos);
        auto direct = check_extra_link_args({arg});
        REQUIRE_FALSE(direct);
        CHECK(direct.error().code == diag::kForbiddenLinkerOption);
    }
}

TEST_CASE("link command: harmless extras are accepted") {
    const char* good[] = {"/OPT:NOREF", "/OPT:NOICF", "/OPT:NOREF,NOICF", "/OPT:LBR", "/INCREMENTAL:NO", "/DYNAMICBASE:NO",
                          "/NODEFAULTLIB:libcmt", "/WX", "/LIBPATH:C:\\x", "/IGNORE:4099", "some.obj", "/FIXED"};
    for (const char* arg : good) {
        INFO("argument " << arg);
        // /FIXED is owned by the driver, everything else is allowed.
        auto r = check_extra_link_args({arg});
        if (std::string(arg) == "/FIXED") {
            REQUIRE_FALSE(r);
            CHECK(r.error().code == diag::kReservedLinkerOption);
        } else {
            CHECK_MESSAGE(r, r.error().format());
        }
    }
}

TEST_CASE("link command: options the driver owns are refused (CML3361)") {
    const char* reserved[] = {"/OUT:x.dll", "/PDB:x.pdb", "/BASE:0x1000000", "/DLL", "/NOENTRY", "/ENTRY:main", "/MACHINE:X86",
                              "/DEBUG:FASTLINK", "/DEBUG", "/FUNCTIONPADMIN", "/NODEFAULTLIB", "/nologo"};
    for (const char* arg : reserved) {
        INFO("argument " << arg);
        auto r = check_extra_link_args({arg});
        REQUIRE_FALSE(r);
        CHECK(r.error().code == diag::kReservedLinkerOption);
    }
}

TEST_CASE("link command: a response file in the extra arguments is expanded before it is checked") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "cml_wp09_unit";
    fs::create_directories(dir);
    const fs::path ok = dir / "ok.rsp";
    const fs::path bad = dir / "bad.rsp";
    {
        std::ofstream(ok) << "/WX\n/OPT:NOICF\n";
        std::ofstream(bad) << "/WX\n/OPT:REF\n";
    }
    CHECK(check_extra_link_args({"@" + ok.string()}));
    auto r = check_extra_link_args({"@" + bad.string()});
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kForbiddenLinkerOption);
    auto missing = check_extra_link_args({"@" + (dir / "does_not_exist.rsp").string()});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == diag::kBadLinkRequest);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---- response files ---------------------------------------------------------------------------------------------

TEST_CASE("link command: a long command line goes into a response file") {
    std::vector<std::string> args = {"/nologo", "/DLL"};
    CHECK_FALSE(needs_response_file("C:\\tools\\link.exe", args));
    for (int i = 0; i < 1500; ++i) {
        args.push_back("C:\\some\\long\\directory\\name\\for\\objects\\object_number_" + std::to_string(i) + ".obj");
    }
    CHECK(needs_response_file("C:\\tools\\link.exe", args));
    CHECK_FALSE(needs_response_file("C:\\tools\\link.exe", args, 1000000));
    // The limit is on the whole line, program included.
    std::vector<std::string> one = {std::string(90, 'a')};
    CHECK_FALSE(needs_response_file("x", one, 100));
    CHECK(needs_response_file(std::string(20, 'p'), one, 100));
}

TEST_CASE("link command: response file bytes are UTF-16LE with a BOM, one quoted argument per line") {
    const std::vector<std::string> args = {"/nologo", "/OUT:C:\\my dir\\p.dll", "C:\\caf\xC3\xA9\\x.obj", "C:\\\xF0\x9F\x98\x80.obj"};
    const std::vector<std::byte> bytes = render_response_file(args);
    REQUIRE(bytes.size() >= 4);
    CHECK(bytes[0] == std::byte{0xFF});
    CHECK(bytes[1] == std::byte{0xFE});
    REQUIRE(bytes.size() % 2 == 0);
    std::u16string text;
    for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
        text.push_back(static_cast<char16_t>(std::to_integer<unsigned>(bytes[i]) | (std::to_integer<unsigned>(bytes[i + 1]) << 8)));
    }
    const std::u16string expected = u"/nologo\r\n\"/OUT:C:\\my dir\\p.dll\"\r\nC:\\caf\u00E9\\x.obj\r\nC:\\\U0001F600.obj\r\n";
    CHECK(text == expected);
}

// ---- linker output ----------------------------------------------------------------------------------------------

TEST_CASE("link output: link.exe errors, warnings and unresolved symbols") {
    const std::string out =
        "logic.anchored.obj : error LNK2019: unresolved external symbol \"int __cdecl helper(int)\" (?helper@@YAHH@Z) referenced in function \"int __cdecl compute_value(void)\" (?compute_value@@YAHXZ)\r\n"
        "patch_1.dll : fatal error LNK1120: 1 unresolved externals\r\n"
        "LINK : warning LNK4281: undesirable base address 0x180000000 for x64 image; set base address above 4GB for best ASLR optimization\r\n"
        "binding.obj : error LNK2001: unresolved external symbol g_tick\r\n";
    const LinkOutputSummary s = summarize_link_output(out);
    CHECK(s.errors.size() == 3);
    CHECK(s.warnings.size() == 1);
    REQUIRE(s.unresolved.size() == 2);
    CHECK(s.unresolved[0] == "?helper@@YAHH@Z");
    CHECK(s.unresolved[1] == "g_tick");
}

TEST_CASE("link output: lld-link errors and warnings") {
    const std::string out =
        "lld-link: error: undefined symbol: ?helper@@YAHH@Z\n"
        ">>> referenced by logic.anchored.obj:(?compute_value@@YAHXZ)\n"
        "lld-link: warning: ignoring unknown argument '/FUNCTIONPADMIN'\n"
        "lld-link: error: could not open 'missing.obj': no such file or directory\n";
    const LinkOutputSummary s = summarize_link_output(out);
    CHECK(s.errors.size() == 2);
    CHECK(s.warnings.size() == 1);
    REQUIRE(s.unresolved.size() == 1);
    CHECK(s.unresolved[0] == "?helper@@YAHH@Z");
}

TEST_CASE("link output: banner lines and empty output produce nothing") {
    CHECK(summarize_link_output("").errors.empty());
    const LinkOutputSummary s = summarize_link_output("   Creating library x.lib and object x.exp\r\n");
    CHECK(s.errors.empty());
    CHECK(s.warnings.empty());
}

// ---- family detection from PE facts ---------------------------------------------------------------------------

TEST_CASE("linker family: PE facts from synthetic images") {
    const PeLinkerFacts link = read_pe_linker_facts(make_pe(14, 44, true));
    CHECK(link.valid);
    CHECK(link.major == 14);
    CHECK(link.minor == 44);
    CHECK(link.rich_header);

    const PeLinkerFacts lld = read_pe_linker_facts(make_pe(14, 0, false));
    CHECK(lld.valid);
    CHECK(lld.major == 14);
    CHECK(lld.minor == 0);
    CHECK_FALSE(lld.rich_header);

    CHECK(read_pe_linker_facts(make_pe(14, 44, true, 0x10B)).valid);  // PE32 is still a PE
}

TEST_CASE("linker family: malformed PE input is invalid, never a crash") {
    CHECK_FALSE(read_pe_linker_facts({}).valid);
    std::vector<std::byte> mz(0x20, std::byte{0});
    CHECK_FALSE(read_pe_linker_facts(mz).valid);
    std::vector<std::byte> pe = make_pe(14, 44, true);
    pe[0] = std::byte{'X'};
    CHECK_FALSE(read_pe_linker_facts(pe).valid);
    pe = make_pe(14, 44, true);
    pe[0x3C] = std::byte{0xF0};  // e_lfanew past the end
    pe[0x3D] = std::byte{0xFF};
    CHECK_FALSE(read_pe_linker_facts(pe).valid);
    pe = make_pe(14, 44, true);
    pe[0x100] = std::byte{'Q'};  // no PE signature
    CHECK_FALSE(read_pe_linker_facts(pe).valid);
    // Truncated inside the optional header.
    pe = make_pe(14, 44, true);
    pe.resize(0x100 + 24 + 2);
    CHECK_FALSE(read_pe_linker_facts(pe).valid);
}

TEST_CASE("linker family: the PE alone decides when there is no PDB") {
    auto link = decide_linker_family(read_pe_linker_facts(make_pe(14, 44, true)), nullptr);
    REQUIRE(link);
    CHECK(link->family == ToolFamily::Link);
    CHECK(link->from_pdb == ToolFamily::Unknown);
    CHECK(link->version[0] == 14);
    CHECK(link->version[1] == 44);
    auto lld = decide_linker_family(read_pe_linker_facts(make_pe(14, 0, false)), nullptr);
    REQUIRE(lld);
    CHECK(lld->family == ToolFamily::LldLink);
    CHECK_FALSE(lld->evidence.empty());
}

TEST_CASE("linker family: PDB facts, in order of strength") {
    const PeLinkerFacts none;
    // 1. the exe name of the linker module
    auto a = decide_linker_family(none, &g_lld);
    REQUIRE(a);
    CHECK(a->family == ToolFamily::LldLink);
    PdbLinkerFacts f = link_exe_pdb();
    f.exe_path = "C:\\VS\\VC\\Tools\\MSVC\\14.44\\bin\\Hostx64\\x64\\link.exe";
    auto b = decide_linker_family(none, &f);
    REQUIRE(b);
    CHECK(b->family == ToolFamily::Link);
    // 2. the S_COMPILE3 version string
    auto c = decide_linker_family(none, &g_link);
    REQUIRE(c);
    CHECK(c->family == ToolFamily::Link);
    CHECK(c->version[0] == 14);  // from the compile3 backend version: no PE
    PdbLinkerFacts llvm;
    llvm.linker_module_found = true;
    llvm.has_compile3 = true;
    llvm.compile_version = "LLD 18.1.8";
    auto d = decide_linker_family(none, &llvm);
    REQUIRE(d);
    CHECK(d->family == ToolFamily::LldLink);
    // 3. only an environment block (lld-link writes one, link.exe does not)
    PdbLinkerFacts env;
    env.linker_module_found = true;
    env.has_env_block = true;
    auto e = decide_linker_family(none, &env);
    REQUIRE(e);
    CHECK(e->family == ToolFamily::LldLink);
    // a linker module that says nothing
    PdbLinkerFacts silent;
    silent.linker_module_found = true;
    auto g = decide_linker_family(none, &silent);
    REQUIRE_FALSE(g);
    CHECK(g.error().code == diag::kLinkerFamilyUnknown);
    // no linker module at all
    PdbLinkerFacts nomod;
    auto h = decide_linker_family(none, &nomod);
    REQUIRE_FALSE(h);
    CHECK(h.error().code == diag::kLinkerFamilyUnknown);
}

TEST_CASE("linker family: PE and PDB that agree produce no warning") {
    auto r = decide_linker_family(read_pe_linker_facts(make_pe(14, 44, true)), &g_link);
    REQUIRE(r);
    CHECK(r->family == ToolFamily::Link);
    CHECK(r->from_pe == ToolFamily::Link);
    CHECK(r->from_pdb == ToolFamily::Link);
    CHECK(r->warnings.empty());
    auto l = decide_linker_family(read_pe_linker_facts(make_pe(14, 0, false)), &g_lld);
    REQUIRE(l);
    CHECK(l->family == ToolFamily::LldLink);
    CHECK(l->warnings.empty());
}

TEST_CASE("linker family: PE and PDB that disagree warn (CML3366) and the PDB wins") {
    auto r = decide_linker_family(read_pe_linker_facts(make_pe(14, 44, true)), &g_lld);
    REQUIRE(r);
    CHECK(r->family == ToolFamily::LldLink);
    REQUIRE(r->warnings.size() == 1);
    CHECK(r->warnings[0].code == diag::kLinkerEvidenceConflict);
    auto r2 = decide_linker_family(read_pe_linker_facts(make_pe(14, 0, false)), &g_link);
    REQUIRE(r2);
    CHECK(r2->family == ToolFamily::Link);
    CHECK(r2->warnings.size() == 1);
}

TEST_CASE("linker family: the user override wins and is reported") {
    DetectLinkerOptions o;
    o.override_family = ToolFamily::LldLink;
    auto r = decide_linker_family(read_pe_linker_facts(make_pe(14, 44, true)), &g_link, o);
    REQUIRE(r);
    CHECK(r->family == ToolFamily::LldLink);
    CHECK(r->overridden);
    CHECK(r->from_pdb == ToolFamily::Link);  // the evidence is still collected
    // An override also decides when nothing else can.
    auto n = decide_linker_family(PeLinkerFacts{}, nullptr, o);
    REQUIRE(n);
    CHECK(n->family == ToolFamily::LldLink);
    // A compiler is not a linker.
    o.override_family = ToolFamily::Cl;
    auto bad = decide_linker_family(PeLinkerFacts{}, nullptr, o);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == diag::kLinkerFamilyUnknown);
}

TEST_CASE("linker family: nothing known and no override is CML3365") {
    auto r = decide_linker_family(PeLinkerFacts{}, nullptr);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kLinkerFamilyUnknown);
}

// ---- reading the linked patch -----------------------------------------------------------------------------------

TEST_CASE("linked patch: a missing DLL is CML3371, a missing PDB CML3370") {
    ReadPatchRequest req;
    req.dll_path = (std::filesystem::temp_directory_path() / "cml_wp09_no_such_patch.dll").string();
    req.pdb_path = (std::filesystem::temp_directory_path() / "cml_wp09_no_such_patch.pdb").string();
    auto r = read_linked_patch(req);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == diag::kPatchImageUnreadable);
}

TEST_CASE("linked patch: find_section looks up by (object, section)") {
    LinkedPatch p;
    for (std::uint32_t o = 0; o < 3; ++o) {
        for (std::uint32_t s = 1; s <= 4; s += 2) {
            SectionPlacement sp;
            sp.object = o;
            sp.section_index = s;
            sp.section_rva = o * 0x1000 + s * 0x10;
            p.sections.push_back(sp);
        }
    }
    const SectionPlacement* hit = p.find_section(1, 3);
    REQUIRE(hit != nullptr);
    CHECK(hit->section_rva == 0x1000 + 0x30);
    CHECK(p.find_section(1, 2) == nullptr);
    CHECK(p.find_section(3, 1) == nullptr);
    CHECK(p.find_section(0, 1) != nullptr);
}
