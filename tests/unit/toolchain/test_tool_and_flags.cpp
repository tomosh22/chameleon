// Toolchain banner parsing, build-flag detection, symbol sets, output parsing, job pool, cancellation.
#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../coff/obj_builder.h"
#include "cml/toolchain/toolchain.h"
#include "doctest.h"

using namespace cml::toolchain;
using cml_test::Bytes;
namespace pdb = cml::pdb;

// ---- tool banners ---------------------------------------------------------------------------

TEST_CASE("tool banners: cl, link, clang-cl, lld-link") {
    auto cl = parse_tool_banner("Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35207.1 for x64\r\nCopyright (C) Microsoft Corporation.  All rights reserved.\r\n");
    CHECK(cl.family == ToolFamily::Cl);
    CHECK(cl.version_text == "19.44.35207.1");
    CHECK(cl.version == std::array<unsigned, 4>{19, 44, 35207, 1});
    CHECK(cl.target == "x64");
    auto x86 = parse_tool_banner("Microsoft (R) C/C++ Optimizing Compiler Version 19.29.30159 for x86");
    CHECK(x86.target == "x86");
    CHECK(x86.version == std::array<unsigned, 4>{19, 29, 30159, 0});

    auto link = parse_tool_banner("Microsoft (R) Incremental Linker Version 14.44.35207.0\r\nCopyright (C) Microsoft Corporation.  All rights reserved.\r\n");
    CHECK(link.family == ToolFamily::Link);
    CHECK(link.version_text == "14.44.35207.0");

    const char* clang_text = "clang version 18.1.8\nTarget: x86_64-pc-windows-msvc\nThread model: posix\nInstalledDir: C:\\Program Files\\LLVM\\bin\n";
    auto clang_cl = parse_tool_banner(clang_text, ToolFamily::ClangCl);
    CHECK(clang_cl.family == ToolFamily::ClangCl);
    CHECK(clang_cl.version == std::array<unsigned, 4>{18, 1, 8, 0});
    CHECK(clang_cl.target == "x86_64-pc-windows-msvc");
    CHECK(clang_cl.major() == 18);
    CHECK(parse_tool_banner(clang_text).family == ToolFamily::Clang);
    CHECK(parse_tool_banner("Ubuntu clang version 18.1.3 (1ubuntu1)\nTarget: x86_64-pc-linux-gnu\n", ToolFamily::Clang).version_text == "18.1.3");

    auto lld = parse_tool_banner("LLD 20.1.8\n");
    CHECK(lld.family == ToolFamily::LldLink);
    CHECK(lld.version_text == "20.1.8");

    auto nothing = parse_tool_banner("this is not a compiler");
    CHECK(nothing.family == ToolFamily::Unknown);
    CHECK(nothing.version_text.empty());
    CHECK(parse_tool_banner("").family == ToolFamily::Unknown);
}

TEST_CASE("tool names and version numbers") {
    CHECK(family_from_path(R"(C:\x\CL.EXE)") == ToolFamily::Cl);
    CHECK(family_from_path(R"(C:\x\clang-cl.exe)") == ToolFamily::ClangCl);
    CHECK(family_from_path("clang.exe") == ToolFamily::Clang);
    CHECK(family_from_path("/usr/bin/clang++") == ToolFamily::Clang);
    CHECK(family_from_path("link") == ToolFamily::Link);
    CHECK(family_from_path("LLD-LINK.EXE") == ToolFamily::LldLink);
    CHECK(family_from_path("gcc") == ToolFamily::Unknown);
    CHECK(std::string(default_file_name(ToolFamily::ClangCl)) == "clang-cl.exe");
    std::array<unsigned, 4> v{};
    CHECK(parse_version_numbers("1.2.3.4-extra", v));
    CHECK(v == std::array<unsigned, 4>{1, 2, 3, 4});
    CHECK(parse_version_numbers("17", v));
    CHECK(v == std::array<unsigned, 4>{17, 0, 0, 0});
    CHECK_FALSE(parse_version_numbers("v1", v));
    CHECK_FALSE(parse_version_numbers("", v));
    auto missing = locate_executable("definitely_not_a_tool_xyz", {});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == diag::kToolNotFound);
}

// ---- object classification and build flags -----------------------------------------------------

namespace {
std::span<const std::byte> span_of(const Bytes& b) { return cml_test::as_span(b); }

Bytes header_bytes(std::initializer_list<unsigned> v) {
    Bytes b;
    for (unsigned x : v) b.push_back(static_cast<std::uint8_t>(x));
    return b;
}

// An object with one function per entry: {name, in COMDAT with the given selection (0 = plain section)}.
Bytes object_with_functions(const std::vector<std::pair<std::string, int>>& fns) {
    using namespace cml_test;
    ObjSpec spec;
    for (const auto& f : fns) {
        SectionSpec s;
        s.name = f.second ? ".text$mn" : ".text";
        s.characteristics = 0x60500020u | (f.second ? 0x1000u : 0u);  // code, exec, read, align16, [comdat]
        s.data = Bytes{0xC3};
        spec.sections.push_back(s);
    }
    std::uint32_t index = 1;
    for (const auto& f : fns) {
        SymbolSpec sec;
        sec.name = f.second ? ".text$mn" : ".text";
        sec.section = static_cast<std::int32_t>(index);
        sec.storage_class = 3;
        sec.aux.push_back(aux_section_def(false, 1, 0, 0, 0, 0, static_cast<std::uint8_t>(f.second)));
        spec.symbols.push_back(sec);
        SymbolSpec fn;
        fn.name = f.first;
        fn.section = static_cast<std::int32_t>(index);
        fn.type = 0x20;
        fn.storage_class = 2;
        spec.symbols.push_back(fn);
        ++index;
    }
    return build_object(spec).bytes;
}

CompilandFlagFacts facts_for(const std::string& cmd, Dialect dialect, const pdb::CompileInfo* c3 = nullptr,
                             const Bytes* obj = nullptr, bool have_cmd = true) {
    const std::vector<std::string> args = split_command_line(cmd);
    CompilandFlagInputs in;
    in.args = args;
    in.dialect = dialect;
    in.have_command_line = have_cmd;
    in.compile3 = c3;
    if (obj) in.object_bytes = span_of(*obj);
    return detect_compiland_flags(in);
}
}  // namespace

TEST_CASE("classify_object") {
    CHECK(classify_object({}) == ObjectKind::TooSmall);
    CHECK(classify_object(span_of(header_bytes({0x64, 0x86, 1}))) == ObjectKind::TooSmall);
    CHECK(classify_object(span_of(header_bytes({0x64, 0x86, 1, 0}))) == ObjectKind::Unknown);  // a cut-off COFF header
    Bytes coff(40, 0);
    coff[0] = 0x64;
    coff[1] = 0x86;
    CHECK(classify_object(span_of(coff)) == ObjectKind::Coff);
    Bytes bigobj(56, 0);
    bigobj[2] = 0xFF;
    bigobj[3] = 0xFF;
    bigobj[4] = 2;
    for (std::size_t i = 0; i < 16; ++i) bigobj[12 + i] = cml_test::kBigobjClassId[i];
    CHECK(classify_object(span_of(bigobj)) == ObjectKind::CoffBigobj);
    Bytes il = bigobj;  // same anonymous header with another class id: cl /GL
    il[12] ^= 0x55;
    CHECK(classify_object(span_of(il)) == ObjectKind::IlLtcg);
    CHECK(classify_object(span_of(header_bytes({'B', 'C', 0xC0, 0xDE, 0, 0}))) == ObjectKind::LlvmBitcode);
    CHECK(classify_object(span_of(header_bytes({0xDE, 0xC0, 0x17, 0x0B, 0, 0}))) == ObjectKind::LlvmBitcode);
    CHECK(classify_object(span_of(header_bytes({'M', 'Z', 0, 0, 0, 0}))) == ObjectKind::Unknown);
    // cml_coff and the classifier agree: it rejects exactly what we call IL.
    CHECK_FALSE(cml::coff::Object::parse(span_of(il)));
}

TEST_CASE("flags: /GL from object format, S_COMPILE3, command line, in that order") {
    Bytes il(56, 0);
    il[2] = 0xFF;
    il[3] = 0xFF;
    il[4] = 2;
    const Bytes coff = object_with_functions({{"f", 0}});
    SUBCASE("IL object") {
        auto f = facts_for("/c /Zi a.cpp", Dialect::Cl, nullptr, &il);
        CHECK(f.whole_program_optimization.value == Tri::Yes);
        CHECK(f.whole_program_optimization.source == Evidence::ObjectFormat);
        CHECK(f.object_kind == ObjectKind::IlLtcg);
    }
    SUBCASE("COFF object beats a stale command line") {
        auto f = facts_for("/c /GL a.cpp", Dialect::Cl, nullptr, &coff);
        CHECK(f.whole_program_optimization.value == Tri::No);
        CHECK(f.whole_program_optimization.source == Evidence::ObjectFormat);
        CHECK(f.whole_program_optimization.detail.find("stale") != std::string::npos);
    }
    SUBCASE("S_COMPILE3 without an object") {
        pdb::CompileInfo c3;
        c3.ltcg = true;
        auto f = facts_for("/c a.cpp", Dialect::Cl, &c3);
        CHECK(f.whole_program_optimization.value == Tri::Yes);
        CHECK(f.whole_program_optimization.source == Evidence::Compile3);
    }
    SUBCASE("command line only") {
        CHECK(facts_for("/c /GL a.cpp", Dialect::Cl).whole_program_optimization.value == Tri::Yes);
        CHECK(facts_for("/c /GL /GL- a.cpp", Dialect::Cl).whole_program_optimization.value == Tri::No);
        CHECK(facts_for("/c a.cpp", Dialect::Cl).whole_program_optimization.source == Evidence::CommandLineAbsent);
        CHECK(facts_for("-cc1 -flto=thin -x c++", Dialect::ClangCc1).whole_program_optimization.value == Tri::Yes);
        CHECK(facts_for("/c -flto a.cpp", Dialect::ClangClDriver).whole_program_optimization.value == Tri::Yes);
    }
    SUBCASE("nothing known") {
        auto f = facts_for("", Dialect::Unknown, nullptr, nullptr, false);
        CHECK(f.whole_program_optimization.value == Tri::Unknown);
        CHECK(f.whole_program_optimization.source == Evidence::None);
    }
}

TEST_CASE("flags: /Gy from the command line, implied options and the object's COMDATs") {
    CHECK(facts_for("/c /Gy a.cpp", Dialect::Cl).function_level_linking.value == Tri::Yes);
    CHECK(facts_for("/c /Gy /Gy- a.cpp", Dialect::Cl).function_level_linking.value == Tri::No);
    CHECK(facts_for("/c /Gy- /Gy a.cpp", Dialect::Cl).function_level_linking.value == Tri::Yes);
    auto zi = facts_for("/c /ZI a.cpp", Dialect::Cl);
    CHECK(zi.function_level_linking.value == Tri::Yes);
    CHECK(zi.function_level_linking.source == Evidence::CommandLineImplied);
    CHECK(facts_for("/c /O2 a.cpp", Dialect::Cl).function_level_linking.value == Tri::Yes);
    CHECK(facts_for("/c /O2 /Gy- a.cpp", Dialect::Cl).function_level_linking.value == Tri::No);
    CHECK(facts_for("/c /O2 a.cpp", Dialect::ClangClDriver).function_level_linking.value == Tri::Yes);  // clang-cl: /O1 and /O2 include /Gy
    CHECK(facts_for("/c /O1 a.cpp", Dialect::ClangClDriver).function_level_linking.source == Evidence::CommandLineImplied);
    CHECK(facts_for("/c /O2 /Gy- a.cpp", Dialect::ClangClDriver).function_level_linking.value == Tri::No);
    CHECK(facts_for("/c /Od a.cpp", Dialect::ClangClDriver).function_level_linking.value == Tri::No);
    CHECK(facts_for("/c /Od a.cpp", Dialect::Cl).function_level_linking.source == Evidence::CommandLineAbsent);
    CHECK(facts_for("-cc1 -ffunction-sections", Dialect::ClangCc1).function_level_linking.value == Tri::Yes);
    CHECK(facts_for("-cc1 -ffunction-sections -fno-function-sections", Dialect::ClangCc1).function_level_linking.value == Tri::No);

    SUBCASE("object layout decides when the command line is silent") {
        const Bytes with_gy = object_with_functions({{"f", 1}, {"g", 1}});
        const Bytes without = object_with_functions({{"f", 0}, {"g", 1}});
        const Bytes inline_only = object_with_functions({{"f", 2}});
        auto a = facts_for("", Dialect::Cl, nullptr, &with_gy, false);
        CHECK(a.function_level_linking.value == Tri::Yes);
        CHECK(a.function_level_linking.source == Evidence::ObjectSections);
        auto b = facts_for("", Dialect::Cl, nullptr, &without, false);
        CHECK(b.function_level_linking.value == Tri::No);
        // Only inline/template COMDATs (selection Any) prove nothing for cl...
        CHECK(facts_for("", Dialect::Cl, nullptr, &inline_only, false).function_level_linking.value == Tri::Unknown);
        // ...but clang uses Any for every function under -ffunction-sections.
        CHECK(facts_for("", Dialect::ClangCc1, nullptr, &inline_only, false).function_level_linking.value == Tri::Yes);
        // An explicit option wins over the object, and the conflict is reported.
        auto c = facts_for("/c /Gy- a.cpp", Dialect::Cl, nullptr, &with_gy);
        CHECK(c.function_level_linking.value == Tri::No);
        CHECK(c.function_level_linking.source == Evidence::CommandLine);
        CHECK(c.function_level_linking.detail.find("object says yes") != std::string::npos);
        // The object beats an implication.
        auto d = facts_for("/c /O2 a.cpp", Dialect::Cl, nullptr, &without);
        CHECK(d.function_level_linking.value == Tri::No);
        CHECK(d.function_level_linking.source == Evidence::ObjectSections);
    }
}

TEST_CASE("flags: /hotpatch and /guard:cf") {
    pdb::CompileInfo set;
    set.hotpatch = true;
    pdb::CompileInfo clear;
    CHECK(facts_for("/c a.cpp", Dialect::Cl, &set).hotpatch.value == Tri::Yes);
    CHECK(facts_for("/c a.cpp", Dialect::Cl, &set).hotpatch.source == Evidence::Compile3);
    CHECK(facts_for("/c a.cpp", Dialect::Cl, &clear).hotpatch.value == Tri::No);
    CHECK(facts_for("/c /hotpatch a.cpp", Dialect::ClangClDriver, &clear).hotpatch.value == Tri::Yes);  // clang may not set the bit
    CHECK(facts_for("/c a.cpp", Dialect::ClangClDriver, &clear).hotpatch.value == Tri::No);
    CHECK(facts_for("-cc1 -fms-hotpatch", Dialect::ClangCc1).hotpatch.value == Tri::Yes);
    CHECK(facts_for("", Dialect::Unknown, nullptr, nullptr, false).hotpatch.value == Tri::Unknown);

    CHECK(facts_for("/c /guard:cf a.cpp", Dialect::Cl).guard_cf.value == Tri::Yes);
    CHECK(facts_for("/c /guard:cf,nochecks a.cpp", Dialect::Cl).guard_cf.value == Tri::Yes);
    CHECK(facts_for("/c /guard:cf /guard:cf- a.cpp", Dialect::Cl).guard_cf.value == Tri::No);
    CHECK(facts_for("/c a.cpp", Dialect::Cl).guard_cf.source == Evidence::CommandLineAbsent);
    CHECK(facts_for("-cc1 -cfguard", Dialect::ClangCc1).guard_cf.value == Tri::Yes);
    CHECK(facts_for("", Dialect::Unknown, nullptr, nullptr, false).guard_cf.value == Tri::Unknown);
}

// ---- symbol sets -----------------------------------------------------------------------------

TEST_CASE("symbol sets: defined / undefined / static, generated labels ignored, diff") {
    using namespace cml_test;
    auto build = [](bool extra_external, bool extra_static) {
        ObjSpec spec;
        SectionSpec text;
        text.name = ".text";
        text.characteristics = 0x60500020u;
        text.data = Bytes{0xC3};
        spec.sections.push_back(text);
        auto add = [&](const std::string& n, std::int32_t sec, std::uint8_t cls, std::uint16_t type = 0) {
            SymbolSpec s;
            s.name = n;
            s.section = sec;
            s.storage_class = cls;
            s.type = type;
            spec.symbols.push_back(s);
        };
        add(".text", 1, 3);
        add("?f@@YAHXZ", 1, 2, 0x20);
        add("$LN3@f", 1, 3);
        add("?g@@YAHXZ", 1, 3, 0x20);  // static function
        add("g_tick", 0, 2);           // undefined external
        if (extra_external) add("?h@@YAHXZ", 1, 2, 0x20);
        if (extra_static) add("?k@@YAHXZ", 1, 3, 0x20);
        return build_object(spec).bytes;
    };
    const Bytes a = build(false, false);
    auto oa = cml::coff::Object::parse(span_of(a));
    REQUIRE(oa);
    const ObjectSymbolSet sa = symbol_set_of(*oa);
    CHECK(sa.defined_external == std::set<std::string>{"?f@@YAHXZ"});
    CHECK(sa.undefined_external == std::set<std::string>{"g_tick"});
    CHECK(sa.defined_static == std::set<std::string>{"?g@@YAHXZ"});  // not ".text", not "$LN3@f"
    CHECK(sa.weak_external.empty());
    CHECK(diff_symbol_sets(sa, sa).empty());

    const Bytes b = build(true, true);
    auto ob = cml::coff::Object::parse(span_of(b));
    REQUIRE(ob);
    const SymbolSetDiff d = diff_symbol_sets(sa, symbol_set_of(*ob));
    CHECK_FALSE(d.empty());
    CHECK(d.only_in_first.empty());
    CHECK(d.only_in_second.size() == 2);
    CHECK(d.to_string().find("defined_external: ?h@@YAHXZ") != std::string::npos);
    CHECK(d.to_string().find("defined_static: ?k@@YAHXZ") != std::string::npos);
}

// ---- compiler output ----------------------------------------------------------------------------

TEST_CASE("parse_tool_output: MSVC, clang and tool-level diagnostics") {
    const std::string out =
        "logic.cpp\r\n"
        "D:\\proj\\src\\logic.cpp(12,5): error C2065: 'x': undeclared identifier\r\n"
        "D:\\proj\\src\\logic.cpp(13): warning C4244: 'return': conversion from 'double' to 'int'\r\n"
        "D:\\proj\\src\\logic.cpp(14) : note: see declaration of 'y'\r\n"
        "D:\\proj\\src\\logic.cpp(20,1): error: use of undeclared identifier 'z'\r\n"
        "D:\\proj\\src\\logic.cpp:30:7: warning: unused variable 'w' [-Wunused-variable]\r\n"
        "D:\\proj\\src\\logic.cpp:31: error: gcc style without column\r\n"
        "cl : Command line error D8016 : '/Gm' and '/ZI' command-line options are incompatible\r\n"
        "cl : Command line warning D9025 : overriding '/Gy' with '/Gy-'\r\n"
        "LINK : fatal error LNK1104: cannot open file 'x.lib'\r\n"
        "fatal error C1083: Cannot open include file: 'nope.h': No such file or directory\r\n"
        "D:\\proj\\a.cpp(1): fatal error C1083: Cannot open include file: 'nope.h'\r\n"
        "1 error generated.\r\n"
        "Generating Code...\r\n";
    const auto d = parse_tool_output(out);
    REQUIRE(d.size() == 10);
    CHECK(d[0].file == R"(D:\proj\src\logic.cpp)");
    CHECK(d[0].line == 12);
    CHECK(d[0].column == 5);
    CHECK(d[0].severity == Severity::Error);
    CHECK(d[0].code == "C2065");
    CHECK(d[0].message == "'x': undeclared identifier");
    CHECK(d[0].format() == R"(D:\proj\src\logic.cpp(12,5): error C2065: 'x': undeclared identifier)");
    CHECK(d[1].severity == Severity::Warning);
    CHECK(d[1].column == 0);
    CHECK(d[1].format() == R"(D:\proj\src\logic.cpp(13): warning C4244: 'return': conversion from 'double' to 'int')");
    CHECK(d[2].severity == Severity::Note);
    CHECK(d[3].code.empty());
    CHECK(d[3].message == "use of undeclared identifier 'z'");
    CHECK(d[4].format() == R"(D:\proj\src\logic.cpp(30,7): warning: unused variable 'w' [-Wunused-variable])");
    CHECK(d[5].line == 31);
    CHECK(d[5].column == 0);
    CHECK(d[6].tool == "cl");
    CHECK(d[6].code == "D8016");
    CHECK(d[6].severity == Severity::Error);
    CHECK(d[7].severity == Severity::Warning);
    CHECK(d[8].tool == "LINK");
    CHECK(d[8].severity == Severity::FatalError);
    CHECK(d[8].format() == "LINK: fatal error LNK1104: cannot open file 'x.lib'");
    CHECK(d[9].file == R"(D:\proj\a.cpp)");
    CHECK(d[9].severity == Severity::FatalError);

    // reformat keeps unrecognised lines and rewrites recognised ones in clickable form
    const std::string re = reformat_tool_output(out);
    CHECK(re.find("logic.cpp\r\n") == std::string::npos);  // CR is normalised away
    CHECK(re.find("Generating Code...\n") != std::string::npos);
    CHECK(re.find(R"(D:\proj\src\logic.cpp(30,7): warning: unused variable 'w' [-Wunused-variable])") != std::string::npos);
    CHECK(parse_tool_output("").empty());
    CHECK(parse_tool_output("nothing: to see here\n: error: no prefix\n").empty());
}

TEST_CASE("parse_show_includes") {
    const auto inc = parse_show_includes("Note: including file: D:\\a\\x.h\r\nNote: including file:   D:\\a\\y.h\r\nother\r\nNote: including file:C:\\z.h");
    CHECK(inc == std::vector<std::string>{R"(D:\a\x.h)", R"(D:\a\y.h)", R"(C:\z.h)"});
}

// ---- cancellation, job pool ----------------------------------------------------------------------

TEST_CASE("CancelToken: killers run once on cancel; late registration runs at once; removal works") {
    CancelToken token;
    int a = 0, b = 0;
    const auto ha = token.add_killer([&] { ++a; });
    const auto hb = token.add_killer([&] { ++b; });
    token.remove_killer(hb);
    CHECK_FALSE(token.cancelled());
    token.cancel();
    token.cancel();
    CHECK(token.cancelled());
    CHECK(a == 1);  // the second cancel() does not run the killers again
    CHECK(b == 0);
    (void)ha;
    int late = 0;
    token.add_killer([&] { ++late; });
    CHECK(late == 1);
}

TEST_CASE("run_parallel: min(jobs, hardware_concurrency) workers, every index once, cancel skips the rest") {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    CHECK(pool_size(0) == 0);
    CHECK(pool_size(1) == 1);
    CHECK(pool_size(1000) == hw);
    CHECK(pool_size(1000, 2) == std::min(2u, hw));
    CHECK(pool_size(3) == std::min<unsigned>(3, hw));

    std::mutex m;
    std::multiset<std::size_t> seen;
    std::atomic<int> running{0}, peak{0};
    run_parallel(50, 4, [&](std::size_t i) {
        const int now = ++running;
        int p = peak.load();
        while (now > p && !peak.compare_exchange_weak(p, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        --running;
        std::lock_guard<std::mutex> lock(m);
        seen.insert(i);
    });
    CHECK(seen.size() == 50);
    CHECK(std::set<std::size_t>(seen.begin(), seen.end()).size() == 50);
    CHECK(peak.load() <= static_cast<int>(pool_size(50, 4)));

    CancelToken token;
    std::atomic<int> ran{0};
    run_parallel(
        1000, 1,
        [&](std::size_t) {
            if (++ran == 5) token.cancel();
        },
        &token);
    CHECK(ran.load() == 5);
    run_parallel(0, 4, [&](std::size_t) { FAIL("no work expected"); });
}
