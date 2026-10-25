// WP-06 acceptance: the writer's output through the real linkers.
//
// For each set of fixture objects (and for every linker: link.exe and lld-link):
//   1. binding.obj is generated for every external symbol the set leaves undefined and linked with
//      the objects as `/DLL /NOENTRY /NODEFAULTLIB` (plan 4.6 command line); the link must succeed
//      (no /FORCE:UNRESOLVED), every thunk must be `FF 25 rel32` landing on its slot, and every slot
//      must be a public in the PDB (read with cml_pdb);
//   2. the same objects with STATIC anchors (the plan's form) are linked and compared with the
//      unanchored link: section layout and all code sections must be byte-identical;
//   3. the same with EXTERNAL tagged anchors (the alternative evaluated for PDB visibility).
// Where the anchors show up in the PDB (publics / module symbols / neither) and how well the
// section contributions reproduce the anchor addresses is printed as EVIDENCE lines and appended to
// <work dir>/evidence.txt (CI prints it).
//
// Environment (local runs only): CML_TEST_LINKERS=lld-link restricts the linkers; CI runs both and
// fails when one is missing.
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "cml/coff/anchors.h"
#include "cml/coff/binding.h"
#include "cml/coff/coff.h"
#include "cml/coff/writer.h"
#include "cml/pdb/pdb.h"
#include "cml/pe/pe.h"
#include "doctest.h"
#include "fixture_paths.h"

namespace fs = std::filesystem;
namespace coff = cml::coff;
using cml_test::FixturePaths;

#ifndef CML_TEST_MSVC_LINK
#define CML_TEST_MSVC_LINK ""
#endif
#ifndef CML_TEST_WORK_DIR
#define CML_TEST_WORK_DIR ""
#endif

namespace {

// ---------------------------------------------------------------------------------------------
// Processes
// ---------------------------------------------------------------------------------------------

struct RunResult {
    int exit_code = -1;
    std::string output;
};

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"'") == std::string::npos) {
        return a;
    }
    std::string out = "\"";
    for (char c : a) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}
#endif

RunResult run_process(const std::string& exe, const std::vector<std::string>& args, const std::string& cwd) {
    RunResult result;
#ifdef _WIN32
    std::string cmd = quote_arg(exe);
    for (const std::string& a : args) {
        cmd += ' ';
        cmd += quote_arg(a);
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        result.output = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd = widen(cmd);
    const std::wstring wcwd = widen(cwd);
    const BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        result.output = "CreateProcess failed (" + std::to_string(GetLastError()) + ") for: " + cmd;
        return result;
    }
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got != 0) {
        result.output.append(buf, got);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    result.exit_code = static_cast<int>(code);
#else
    const auto sh_quote = [](const std::string& a) {
        std::string out = "'";
        for (char c : a) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out += c;
            }
        }
        return out + "'";
    };
    std::string cmd;
    if (!cwd.empty()) {
        cmd += "cd " + sh_quote(cwd) + " && ";
    }
    cmd += sh_quote(exe);
    for (const std::string& a : args) {
        cmd += ' ' + sh_quote(a);
    }
    cmd += " 2>&1";
    std::FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) {
        result.output = "popen failed for: " + cmd;
        return result;
    }
    char buf[8192];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) {
        result.output.append(buf, n);
    }
    const int status = pclose(p);
    result.exit_code = status == -1 ? -1 : (status >> 8) & 0xFF;
#endif
    return result;
}

// ---------------------------------------------------------------------------------------------
// Linkers
// ---------------------------------------------------------------------------------------------

struct Linker {
    std::string label;  // "link" or "lld-link"
    std::string exe;
};

#ifdef _WIN32
std::string search_path(const wchar_t* file) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = SearchPathW(nullptr, file, nullptr, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), out.data(), len, nullptr, nullptr);
    return out;
}
#endif

// getenv without the MSVC deprecation warning.
std::string env_value(const char* name) {
#ifdef _WIN32
    char buf[4096];
    const DWORD n = GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    return n == 0 || n >= sizeof(buf) ? std::string() : std::string(buf, n);
#else
    const char* v = std::getenv(name);
    return v == nullptr ? std::string() : std::string(v);
#endif
}

std::string find_linker(const std::string& label) {
#ifdef _WIN32
    if (label == "link") {
        // The linker CMake found (also works without a developer environment on PATH).
        const std::string configured = CML_TEST_MSVC_LINK;
        if (!configured.empty() && fs::exists(configured)) {
            return fs::path(configured).make_preferred().string();
        }
        // msvc-dev-cmd sets VCToolsInstallDir; the Git-for-Windows `link` (coreutils) must not win.
        const std::string vc = env_value("VCToolsInstallDir");
        if (!vc.empty()) {
            const fs::path p = fs::path(vc) / "bin" / "Hostx64" / "x64" / "link.exe";
            if (fs::exists(p)) {
                return p.string();
            }
        }
        return search_path(L"link.exe");
    }
    const fs::path llvm = "C:\\Program Files\\LLVM\\bin\\lld-link.exe";
    if (fs::exists(llvm)) {
        return llvm.string();
    }
    return search_path(L"lld-link.exe");
#else
    if (label == "lld-link") {
        return "lld-link";
    }
    return {};
#endif
}

std::vector<Linker> linkers_under_test() {
    std::vector<std::string> labels = {"link", "lld-link"};
    const std::string env = env_value("CML_TEST_LINKERS");
    if (!env.empty()) {
        labels.clear();
        std::stringstream ss(env);
        std::string item;
        while (std::getline(ss, item, ',')) {
            if (!item.empty()) {
                labels.push_back(item);
            }
        }
    }
    std::vector<Linker> out;
    for (const std::string& l : labels) {
        const std::string exe = find_linker(l);
        INFO("linker " << l);
        REQUIRE_MESSAGE(!exe.empty(), "linker '" << l << "' not found; CI cells provide link.exe and lld-link");
        out.push_back({l, exe});
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

fs::path work_root() {
    std::string dir = CML_TEST_WORK_DIR;
    fs::path p = dir.empty() ? fs::temp_directory_path() / "cml_wp06_link" : fs::path(dir);
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

void evidence(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("EVIDENCE %s\n", buf);
    std::fflush(stdout);
    std::ofstream f(work_root() / "evidence.txt", std::ios::app);
    f << buf << '\n';
}

std::string stem_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = base.find('.');
    if (dot != std::string::npos) {
        base.resize(dot);
    }
    std::string lower;
    for (char c : base) {
        lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return lower;
}

std::string basename_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The object of list `key` whose file stem (name up to the first '.') is `stem`.
std::string find_object(const char* key, const std::string& stem) {
    const FixturePaths& p = FixturePaths::get();
    REQUIRE(p.loaded());
    const std::string list = p.value(key);
    std::size_t start = 0;
    while (start <= list.size()) {
        std::size_t end = list.find(';', start);
        if (end == std::string::npos) {
            end = list.size();
        }
        const std::string path = list.substr(start, end - start);
        if (!path.empty() && stem_of(path) == stem) {
            return path;
        }
        start = end + 1;
    }
    FAIL("fixture object '" << stem << "' not found in list " << key << ": " << list);
    return {};
}

bool write_file(const fs::path& path, const coff::ByteVector& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

// Undefined external symbols the set leaves unresolved, each classified as function or data:
// function when the compiler typed it as one, or when a branch (E8/E9 rel32) or an image-relative
// (ADDR32NB: .xdata handlers, .pdata) reference targets it; data otherwise.
std::vector<coff::BindSymbol> compute_bind_list(const std::vector<coff::OwnedObject>& objects) {
    std::unordered_set<std::string> defined;
    for (const coff::OwnedObject& o : objects) {
        for (const coff::Symbol& s : o.object.symbols()) {
            if ((s.is_external() || s.is_weak_external()) && (s.section_number != 0 || s.is_weak_external() || s.is_common())) {
                defined.insert(std::string(s.name));
            }
            if (s.is_absolute() && s.is_external()) {
                defined.insert(std::string(s.name));
            }
        }
    }
    std::unordered_set<std::string> is_fn;
    for (const coff::OwnedObject& o : objects) {
        for (const coff::Section& sec : o.object.sections()) {
            if (sec.relocation_count == 0) {
                continue;
            }
            auto rs = o.object.resolve_relocations(sec);
            if (!rs) {
                continue;
            }
            for (const coff::ResolvedRelocation& r : *rs) {
                bool fn = r.reloc.type == coff::RelocType::Addr32Nb;
                if (!fn && sec.is_code() && r.reloc.type == coff::RelocType::Rel32 && r.reloc.virtual_address >= 1 &&
                    r.reloc.virtual_address - 1 < sec.data.size()) {
                    const auto op = std::to_integer<unsigned>(sec.data[r.reloc.virtual_address - 1]);
                    fn = op == 0xE8 || op == 0xE9;
                }
                if (fn) {
                    is_fn.insert(std::string(r.target->name));
                }
            }
        }
    }
    std::vector<coff::BindSymbol> out;
    std::unordered_set<std::string> seen;
    for (const coff::OwnedObject& o : objects) {
        for (const coff::Symbol& s : o.object.symbols()) {
            if (!s.is_external() || !s.is_undefined()) {
                continue;
            }
            const std::string name(s.name);
            if (defined.count(name) != 0 || name == "__ImageBase" || !seen.insert(name).second) {
                continue;
            }
            const bool fn = s.is_function() || is_fn.count(name) != 0;
            out.push_back({name, fn ? coff::BindKind::Function : coff::BindKind::Data});
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Link + inspect
// ---------------------------------------------------------------------------------------------

struct Linked {
    bool ok = false;
    std::string dll, pdb, output;
};

// Every variant of one linker links into its own directory under the same file name, and the
// directory names have equal length: the PDB path is embedded in the image (RSDS record), so
// different path lengths would shift the .rdata layout and make the images incomparable.
Linked do_link(const Linker& lk, const fs::path& variant_dir, const std::vector<std::string>& objs) {
    Linked r;
    std::error_code mk;
    fs::create_directories(variant_dir, mk);
    const fs::path dir = variant_dir;
    r.dll = (dir / "image.dll").string();
    r.pdb = (dir / "image.pdb").string();
    std::error_code ec;
    fs::remove(r.dll, ec);
    fs::remove(r.pdb, ec);
    std::vector<std::string> args = {"/nologo", "/DLL", "/NOENTRY", "/NODEFAULTLIB", "/MACHINE:X64", "/DEBUG:FULL",
                                     "/INCREMENTAL:NO", "/OPT:NOREF", "/OPT:NOICF", "/FUNCTIONPADMIN", "/FIXED",
                                     "/DYNAMICBASE:NO", "/BASE:0x180000000", "/BREPRO", "/PDB:" + r.pdb, "/OUT:" + r.dll,
                                     "/IGNORE:4001,4254,4281"};
    for (const std::string& o : objs) {
        args.push_back(o);
    }
    const RunResult run = run_process(lk.exe, args, dir.string());
    r.output = run.output;
    r.ok = run.exit_code == 0 && fs::exists(r.dll) && fs::exists(r.pdb);
    return r;
}

struct SectionDiff {
    bool ok = false;
    std::size_t bytes_differing_outside_debug_info = 0;  // whole file, minus the masked regions below
    std::string first_difference_in;                      // section holding the first such byte
    bool layout_equal = true;  // names, RVAs, sizes, characteristics
    bool code_equal = true;
    bool whole_equal = false;
    std::vector<std::string> data_diffs;  // non-code sections whose bytes differ
    std::string error;
};

SectionDiff compare_images(const std::string& a_path, const std::string& b_path) {
    SectionDiff d;
    auto a = cml::pe::load_image_file(a_path);
    auto b = cml::pe::load_image_file(b_path);
    if (!a || !b) {
        d.error = !a ? a.error().format() : b.error().format();
        return d;
    }
    d.ok = true;
    d.whole_equal = a->bytes == b->bytes;
    if (a->bytes.size() == b->bytes.size()) {
        // Mask what legitimately differs between two links of equal content: the PE timestamp and
        // checksum, and the debug directory with its records (GUID, PDB path, /Brepro hash).
        std::vector<bool> masked(a->bytes.size(), false);
        const auto mask = [&](std::uint64_t off, std::uint64_t len) {
            for (std::uint64_t i = off; i < off + len && i < masked.size(); ++i) {
                masked[static_cast<std::size_t>(i)] = true;
            }
        };
        const std::uint64_t nt = a->image.nt_headers_offset();
        mask(nt + 8, 4);
        mask(nt + 24 + 64, 4);
        const cml::pe::DataDirectory dd = a->image.data_directory(cml::pe::dir::Debug);
        if (dd.rva != 0) {
            if (const auto off = a->image.rva_to_offset(dd.rva)) {
                mask(*off, dd.size);
            }
            if (auto entries = a->image.debug_entries()) {
                for (const cml::pe::DebugEntry& e : *entries) {
                    mask(e.pointer_to_raw_data, e.size_of_data);
                }
            }
        }
        for (std::size_t i = 0; i < masked.size(); ++i) {
            if (!masked[i] && a->bytes[i] != b->bytes[i]) {
                if (d.bytes_differing_outside_debug_info++ == 0) {
                    const auto rva = a->image.offset_to_rva(i);
                    const auto* sec = rva ? a->image.section_for_rva(*rva) : nullptr;
                    d.first_difference_in = sec != nullptr ? std::string(sec->name()) : "headers";
                }
            }
        }
    } else {
        d.bytes_differing_outside_debug_info = ~std::size_t{0};
        d.first_difference_in = "(files differ in size)";
    }
    const auto sa = a->image.sections();
    const auto sb = b->image.sections();
    if (sa.size() != sb.size()) {
        d.layout_equal = false;
        return d;
    }
    // A section range outside the file is a failure of the test (never an empty "equal" section).
    const auto bytes_of = [&d](const cml::pe::OwnedImage& img, const cml::pe::SectionHeader& sh, std::vector<std::byte>* out) {
        const std::size_t off = sh.pointer_to_raw_data;
        const std::size_t len = sh.size_of_raw_data;
        if (len != 0 && (off > img.bytes.size() || len > img.bytes.size() - off)) {
            d.ok = false;
            d.error = "section '" + std::string(sh.name()) + "' has raw data (offset " + std::to_string(off) + ", size " +
                      std::to_string(len) + ") outside the " + std::to_string(img.bytes.size()) + "-byte image";
            return false;
        }
        out->assign(img.bytes.begin() + static_cast<std::ptrdiff_t>(off),
                    img.bytes.begin() + static_cast<std::ptrdiff_t>(off + len));
        return true;
    };
    for (std::size_t i = 0; i < sa.size(); ++i) {
        if (sa[i].raw_name != sb[i].raw_name || sa[i].virtual_address != sb[i].virtual_address ||
            sa[i].virtual_size != sb[i].virtual_size || sa[i].size_of_raw_data != sb[i].size_of_raw_data ||
            sa[i].characteristics != sb[i].characteristics) {
            d.layout_equal = false;
            continue;
        }
        std::vector<std::byte> ba;
        std::vector<std::byte> bb;
        if (!bytes_of(*a, sa[i], &ba) || !bytes_of(*b, sb[i], &bb)) {
            return d;
        }
        const bool same = ba == bb;
        if ((sa[i].characteristics & coff::scn::CntCode) != 0) {
            d.code_equal = d.code_equal && same;
        } else if (!same) {
            d.data_diffs.push_back(std::string(sa[i].name()));
        }
    }
    return d;
}

struct PdbFacts {
    bool ok = false;
    std::string error;
    std::shared_ptr<const cml::pdb::PdbIndex> index;
    std::unordered_map<std::string, std::uint32_t> public_rva;  // name -> rva
    std::unordered_map<std::string, bool> public_is_function;
};

PdbFacts read_pdb(const std::string& path) {
    PdbFacts f;
    cml::pdb::OpenOptions opt;
    opt.use_cache = false;
    auto idx = cml::pdb::PdbIndex::open_pdb(path, opt);
    if (!idx) {
        f.error = idx.error().format();
        return f;
    }
    f.index = *idx;
    auto pubs = f.index->all_publics();
    if (!pubs) {
        f.error = pubs.error().format();
        return f;
    }
    f.public_rva.reserve(pubs->size());
    for (const cml::pdb::SymbolView& s : *pubs) {
        f.public_rva.emplace(std::string(s.name), s.rva);
        f.public_is_function.emplace(std::string(s.name), s.is_function());
    }
    f.ok = true;
    return f;
}

// Anchor-like names found among a compiland's symbols (module symbols).
std::size_t count_module_anchors(const PdbFacts& pdb, const std::string& obj_basename) {
    std::size_t n = 0;
    for (std::uint32_t ci : pdb.index->compilands_by_object_name(obj_basename)) {
        auto syms = pdb.index->compiland_symbols(ci);
        if (!syms) {
            continue;
        }
        for (const cml::pdb::SymbolView& s : *syms) {
            if (s.name.substr(0, coff::kAnchorPrefix.size()) == coff::kAnchorPrefix) {
                ++n;
            }
        }
    }
    return n;
}

struct SetSpec {
    std::string name;
    std::vector<std::pair<const char*, std::string>> objects;  // {fixture list key, file stem}
};

// ---------------------------------------------------------------------------------------------
// Synthetic objects that share a COMDAT (built with the writer itself)
// ---------------------------------------------------------------------------------------------

// Object `which` ("a" or "b"): both define the COMDAT functions/data `shared_fn` / `shared_const`
// (selection Any, identical bytes) which the linker keeps once; each also has its own COMDAT function,
// an associative section, a COMDAT with a static leader, plain .data and .bss, and references the
// undefined externals `ext_f` (call) and `ext_d` (data pointer) that binding.obj has to provide.
coff::ByteVector make_shared_comdat_object(const std::string& which) {
    namespace sc = coff::scn;
    coff::ObjectModel m = coff::ObjectModel::make_empty(coff::Machine::Amd64);
    const std::uint32_t code = sc::CntCode | sc::MemExecute | sc::MemRead | sc::LnkComdat | (5u << sc::AlignShift);
    const std::uint32_t rdata = sc::CntInitializedData | sc::MemRead | sc::LnkComdat | (4u << sc::AlignShift);
    const std::uint32_t data = sc::CntInitializedData | sc::MemRead | sc::MemWrite | (4u << sc::AlignShift);
    const auto bytes_of = [](std::initializer_list<unsigned> v) {
        coff::ByteVector b;
        for (unsigned x : v) {
            b.push_back(static_cast<std::byte>(x));
        }
        b.resize(16, std::byte{0xCC});
        return b;
    };
    // Symbol indices (primary + aux), in the order added below:
    //  0 .text$mn(1) +1   2 shared_fn   3 .text$mn(2) +4   5 unique_<w>   6 .xdata(3) +7   8 .rdata(4) +9
    //  10 shared_const   11 .text$mn(5) +12   13 static_fn   14 .data(6) +15   16 .bss(7) +17
    //  18 ext_f   19 ext_d   20 data_<w>
    auto s1 = m.add_section(".text$mn", code, bytes_of({0xC3}));                                  // shared_fn
    auto s2 = m.add_section(".text$mn", code, bytes_of({0xE8, 0, 0, 0, 0, 0xC3}), {{1, 18, 4}});  // unique: call ext_f
    auto s3 = m.add_section(".xdata", sc::CntInitializedData | sc::MemRead | sc::LnkComdat | (2u << sc::AlignShift),
                            coff::ByteVector(8, std::byte{0}));                                    // associative -> 2
    auto s4 = m.add_section(".rdata", rdata, coff::ByteVector(8, std::byte{0x11}));                // shared_const
    auto s5 = m.add_section(".text$mn", code, bytes_of({0xC3}));                                  // static leader
    auto s6 = m.add_section(".data", data, coff::ByteVector(8, std::byte{0}), {{0, 19, 1}});      // ADDR64 ext_d
    (void)s1; (void)s2; (void)s3; (void)s4; (void)s5; (void)s6;
    coff::ModelSection bss;
    bss.name = ".bss";
    std::memcpy(bss.raw_name.data(), ".bss", 4);
    bss.characteristics = sc::CntUninitializedData | sc::MemRead | sc::MemWrite | (4u << sc::AlignShift);
    bss.size_of_raw_data = 16;
    m.sections.push_back(bss);

    const auto add = [&](const std::string& n, std::uint32_t value, std::int32_t sec, std::uint16_t type, coff::StorageClass cls,
                         coff::ByteVector aux = {}) {
        auto r = m.add_symbol(n, value, sec, type, cls, std::move(aux));
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
    };
    const auto def = [](std::uint32_t len, std::uint32_t relocs, std::uint32_t assoc, std::uint8_t sel) {
        return coff::make_section_definition_aux(false, len, relocs, 0, assoc, sel);
    };
    using C = coff::StorageClass;
    add(".text$mn", 0, 1, 0, C::Static, def(16, 0, 0, 2));
    add("shared_fn", 0, 1, 0x20, C::External);
    add(".text$mn", 0, 2, 0, C::Static, def(16, 1, 0, 2));
    add("unique_" + which, 0, 2, 0x20, C::External);
    add(".xdata", 0, 3, 0, C::Static, def(8, 0, 2, 5));
    add(".rdata", 0, 4, 0, C::Static, def(8, 0, 0, 2));
    add("shared_const", 0, 4, 0, C::External);
    add(".text$mn", 0, 5, 0, C::Static, def(16, 0, 0, 2));
    add("static_fn_" + which, 0, 5, 0x20, C::Static);
    add(".data", 0, 6, 0, C::Static, def(8, 1, 0, 0));
    add(".bss", 0, 7, 0, C::Static, def(16, 0, 0, 0));
    add("ext_f", 0, 0, 0x20, C::External);
    add("ext_d", 0, 0, 0, C::External);
    add("data_" + which, 0, 6, 0, C::External);
    auto bytes = coff::write(m);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error().format());
    return std::move(*bytes);
}

// ---------------------------------------------------------------------------------------------
// One link set
// ---------------------------------------------------------------------------------------------

void check_binding_in_image(const char* where, const Linked& linked, const PdbFacts& pdb,
                            const std::vector<coff::BindSymbol>& bind) {
    INFO(where);
    auto img = cml::pe::load_image_file(linked.dll);
    REQUIRE_MESSAGE(img.has_value(), img.error().format());
    std::size_t functions = 0;
    std::size_t datas = 0;
    for (const coff::BindSymbol& s : bind) {
        INFO("bound symbol " << s.name);
        const auto it = pdb.public_rva.find(s.name);
        REQUIRE_MESSAGE(it != pdb.public_rva.end(), "bound symbol '" << s.name << "' is not a public in the PDB");
        if (s.kind == coff::BindKind::Data) {
            ++datas;
            const auto* sec = img->image.section_for_rva(it->second);
            CHECK_MESSAGE(sec != nullptr, "data placeholder '" << s.name << "' is outside every section");
            continue;
        }
        ++functions;
        const auto slot = pdb.public_rva.find(coff::slot_name(s.name));
        REQUIRE_MESSAGE(slot != pdb.public_rva.end(), "slot of '" << s.name << "' is not a public in the PDB");
        const auto* slot_sec = img->image.section_for_rva(slot->second);
        REQUIRE(slot_sec != nullptr);
        CHECK((slot_sec->characteristics & coff::scn::MemWrite) != 0);
        CHECK(slot->second % coff::kSlotSize == 0);
        // The thunk: FF 25 rel32, rel32 resolved by the linker to the slot.
        auto bytes = img->image.read_rva(it->second, 6);
        REQUIRE_MESSAGE(bytes.has_value(), bytes.error().format());
        CHECK(((*bytes)[0]) == std::byte{0xFF});
        CHECK(((*bytes)[1]) == std::byte{0x25});
        std::int32_t disp = 0;
        std::memcpy(&disp, bytes->data() + 2, 4);
        const std::int64_t target = static_cast<std::int64_t>(it->second) + 6 + disp;
        CHECK_MESSAGE(target == static_cast<std::int64_t>(slot->second),
                      "thunk of '" << s.name << "' at rva " << it->second << " jumps to rva " << target << ", slot is at " << slot->second);
        const auto fn = pdb.public_is_function.find(s.name);
        const bool flagged = fn != pdb.public_is_function.end() && fn->second;
        CHECK_MESSAGE(flagged, "thunk '" << s.name << "' is not flagged as a function public");
    }
    evidence("%s: %zu thunks verified (FF 25 -> slot, slot is a writable public), %zu data placeholders are publics", where, functions, datas);
}

// Links `originals` (object files) as unanchored and as anchored objects, per linker, and checks:
//   * binding.obj satisfies the linker and every thunk/slot is right;
//   * the anchored image equals the unanchored one in every byte except the timestamp/checksum and
//     the debug directory (so in every section: .text .rdata .data .pdata .xdata ...);
//   * every section locator resolves through cml_pdb::find_public-equivalent publics and matches a
//     section contribution of the PDB (start, and size and compiland unless the COMDAT is shared).
// `static_probe`: additionally links a variant whose anchors are flipped to STATIC and asserts that
// none of them is visible anywhere in the PDB (the ADR-0020 finding).
void run_files(const std::string& name, const std::vector<std::string>& originals, bool static_probe) {
    const std::vector<Linker> linkers = linkers_under_test();
    const fs::path dir = work_root() / name;
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }

    // ---- objects, bind list, binding object ----
    std::vector<coff::OwnedObject> objects;
    for (const std::string& path : originals) {
        auto o = coff::load_object_file(path);
        REQUIRE_MESSAGE(o.has_value(), o.error().format());
        objects.push_back(std::move(*o));
    }
    const std::vector<coff::BindSymbol> bind = compute_bind_list(objects);
    std::size_t bind_functions = 0;
    for (const coff::BindSymbol& s : bind) {
        bind_functions += s.kind == coff::BindKind::Function ? 1 : 0;
    }
    auto binding = coff::make_binding_object(bind);
    REQUIRE_MESSAGE(binding.has_value(), binding.error().format());
    const fs::path binding_path = dir / "binding.obj";
    REQUIRE(write_file(binding_path, *binding));

    // ---- anchored copies (one tag set for the whole link) ----
    std::unordered_set<std::string> used_tags;
    std::vector<std::string> plain_files = originals;
    std::vector<std::string> anchored_files;
    std::vector<std::string> probe_files;
    std::vector<std::vector<coff::SectionLocator>> locators;
    std::unordered_map<std::string, std::size_t> leader_copies;  // COMDAT leader name -> number of objects defining it
    for (std::size_t i = 0; i < objects.size(); ++i) {
        auto m = coff::ObjectModel::from_object(objects[i].object);
        REQUIRE_MESSAGE(m.has_value(), m.error().format());
        coff::AnchorOptions opt;
        opt.used_tags = &used_tags;
        auto loc = coff::add_anchors(*m, opt);
        REQUIRE_MESSAGE(loc.has_value(), loc.error().format());
        for (const coff::SectionLocator& l : *loc) {
            if (l.kind == coff::LocatorKind::ComdatLeader) {
                ++leader_copies[l.symbol_name];
            }
        }
        auto bytes = coff::write(*m);
        REQUIRE_MESSAGE(bytes.has_value(), bytes.error().format());
        const std::string file = (dir / (std::to_string(i) + "_" + stem_of(originals[i]) + ".obj")).string();
        REQUIRE(write_file(file, *bytes));
        anchored_files.push_back(file);
        locators.push_back(std::move(*loc));
        if (static_probe) {
            for (coff::ModelSymbol& s : m->symbols) {
                if (s.name.compare(0, coff::kAnchorPrefix.size(), coff::kAnchorPrefix) == 0) {
                    s.storage_class = static_cast<std::uint8_t>(coff::StorageClass::Static);
                }
            }
            auto pb = coff::write(*m);
            REQUIRE(pb.has_value());
            const std::string pfile = (dir / (std::to_string(i) + "_" + stem_of(originals[i]) + "_s.obj")).string();
            REQUIRE(write_file(pfile, *pb));
            probe_files.push_back(pfile);
        }
    }
    std::size_t shared_leaders = 0;
    for (const auto& [leader, copies] : leader_copies) {
        shared_leaders += copies > 1 ? 1 : 0;
    }
    plain_files.push_back(binding_path.string());
    anchored_files.push_back(binding_path.string());
    probe_files.push_back(binding_path.string());

    for (const Linker& lk : linkers) {
        INFO("set " << name << ", linker " << lk.label);
        const std::string tag = name + "/" + lk.label;
        const fs::path ldir = dir / lk.label;

        // 1. unanchored link with the binding object.
        const Linked lp = do_link(lk, ldir / "p0", plain_files);
        CHECK_MESSAGE(lp.ok, "link of the unanchored objects + binding.obj failed:\n" << lp.output);
        REQUIRE(lp.ok);
        for (const char* needle : {"LNK2001", "LNK2019", "unresolved external", "undefined symbol"}) {
            CHECK_MESSAGE(lp.output.find(needle) == std::string::npos, "linker output mentions '" << needle << "':\n" << lp.output);
        }
        {
            PdbFacts pdb = read_pdb(lp.pdb);
            REQUIRE_MESSAGE(pdb.ok, pdb.error);
            check_binding_in_image(tag.c_str(), lp, pdb, bind);
        }

        // 2. anchored link: identical image, locators resolve.
        const Linked la = do_link(lk, ldir / "a0", anchored_files);
        CHECK_MESSAGE(la.ok, "anchored link failed:\n" << la.output);
        REQUIRE(la.ok);
        const SectionDiff diff = compare_images(lp.dll, la.dll);
        REQUIRE_MESSAGE(diff.ok, diff.error);
        CHECK_MESSAGE(diff.layout_equal, tag << ": section table (names, RVAs, sizes, characteristics) differs from the unanchored link");
        CHECK_MESSAGE(diff.code_equal, tag << ": a code section differs from the unanchored link");
        CHECK_MESSAGE(diff.bytes_differing_outside_debug_info == 0,
                      tag << ": " << diff.bytes_differing_outside_debug_info << " bytes differ from the unanchored image outside the "
                          << "debug directory and timestamp/checksum, first in section '" << diff.first_difference_in << "'");

        PdbFacts pdb = read_pdb(la.pdb);
        REQUIRE_MESSAGE(pdb.ok, pdb.error);
        // Section contributions by start RVA.
        std::unordered_multimap<std::uint32_t, cml::pdb::SectionContribution> by_start;
        for (const cml::pdb::SectionContribution& c : pdb.index->contributions()) {
            by_start.emplace(c.rva, c);
        }
        std::size_t anchors = 0, leaders = 0, resolved = 0, start_ok = 0, size_ok = 0, compiland_ok = 0, shared_resolved = 0, no_loc = 0;
        std::size_t exact_checked = 0, failures_shown = 0;
        std::map<std::string, std::size_t> no_loc_reasons;
        for (std::size_t i = 0; i < objects.size(); ++i) {
            const auto compilands = pdb.index->compilands_by_object_name(basename_of(anchored_files[i]));
            for (const coff::SectionLocator& l : locators[i]) {
                if (l.kind == coff::LocatorKind::None) {
                    ++no_loc;
                    ++no_loc_reasons[l.reason == coff::NoLocatorReason::NotLinked      ? "not-linked"
                                 : l.reason == coff::NoLocatorReason::Associative ? "associative"
                                 : l.reason == coff::NoLocatorReason::NoLeader    ? "no-leader"
                                                                                  : "static-leader"];
                    continue;
                }
                (l.kind == coff::LocatorKind::Anchor ? anchors : leaders) += 1;
                const auto pub = pdb.public_rva.find(l.symbol_name);
                const bool found = pub != pdb.public_rva.end();
                CHECK_MESSAGE(found, tag << ": locator '" << l.symbol_name << "' (section " << l.section_index << " of object " << i
                                         << ") is not a public in the PDB");
                if (!found) {
                    continue;
                }
                ++resolved;
                const coff::Section* sec = objects[i].object.section(l.section_index);
                const bool shared = l.kind == coff::LocatorKind::ComdatLeader && leader_copies[l.symbol_name] > 1;
                shared_resolved += shared ? 1 : 0;
                if (sec == nullptr || sec->size_of_raw_data == 0 || pub->second < l.offset) {
                    continue;
                }
                const std::uint32_t start = pub->second - l.offset;
                const auto range = by_start.equal_range(start);
                bool has_start = range.first != range.second;
                bool has_size = false;
                bool has_compiland = false;
                for (auto it = range.first; it != range.second; ++it) {
                    has_size = has_size || it->second.size == sec->size_of_raw_data;
                    has_compiland = has_compiland || (!compilands.empty() && it->second.compiland == compilands.front());
                }
                start_ok += has_start ? 1 : 0;
                size_ok += has_size ? 1 : 0;
                compiland_ok += has_compiland ? 1 : 0;
                ++exact_checked;
                // A COMDAT kept once for several objects: the start must exist; its size/compiland belong to the kept copy.
                const bool ok = shared ? has_start : (has_start && has_size && has_compiland);
                CHECK_MESSAGE(ok, tag << ": locator '" << l.symbol_name << "' (section " << l.section_index << " '" << std::string(sec->name)
                                      << "' of object " << i << ") start rva " << start << ": contribution at start=" << has_start
                                      << " same size=" << has_size << " same compiland=" << has_compiland);
                if (!ok && failures_shown++ < 5) {
                    evidence("MISMATCH %s section=%u '%s' raw_size=%u locator_rva=%u offset=%u", tag.c_str(), static_cast<unsigned>(l.section_index),
                             std::string(sec->name).c_str(), static_cast<unsigned>(sec->size_of_raw_data), static_cast<unsigned>(pub->second),
                             static_cast<unsigned>(l.offset));
                }
            }
        }
        std::string reasons;
        for (const auto& [r, n] : no_loc_reasons) {
            reasons += " " + r + "=" + std::to_string(n);
        }
        evidence("set=%s linker=%s | link ok, bound=%zu (%zu functions) | image identical outside timestamp/checksum/debug directory: %s "
                 "(layout %s, code %s, %zu bytes differ) | locators: %zu anchors + %zu comdat leaders resolved %zu/%zu in PDB publics, "
                 "%zu without locator (reasons:%s) | section contributions with same start %zu, same size %zu, same compiland %zu of %zu checked | "
                 "shared COMDAT leaders in this link: %zu (locators of them resolved: %zu)",
                 name.c_str(), lk.label.c_str(), bind.size(), bind_functions, diff.bytes_differing_outside_debug_info == 0 ? "YES" : "NO",
                 diff.layout_equal ? "identical" : "DIFFERENT", diff.code_equal ? "identical" : "DIFFERENT",
                 diff.bytes_differing_outside_debug_info, anchors, leaders, resolved, anchors + leaders, no_loc, reasons.c_str(), start_ok,
                 size_ok, compiland_ok, exact_checked, shared_leaders, shared_resolved);

        // 3. the static probe: STATIC anchors are invisible in the PDB.
        if (static_probe) {
            pdb.index.reset();
            const Linked ls = do_link(lk, ldir / "s0", probe_files);
            CHECK_MESSAGE(ls.ok, "static-anchor link failed:\n" << ls.output);
            REQUIRE(ls.ok);
            PdbFacts spdb = read_pdb(ls.pdb);
            REQUIRE_MESSAGE(spdb.ok, spdb.error);
            std::size_t in_publics = 0;
            std::size_t in_module = 0;
            for (std::size_t i = 0; i < objects.size(); ++i) {
                in_module += count_module_anchors(spdb, basename_of(probe_files[i]));
                for (const coff::SectionLocator& l : locators[i]) {
                    if (l.kind == coff::LocatorKind::Anchor) {
                        in_publics += spdb.public_rva.count(l.symbol_name);
                    }
                }
            }
            CHECK_MESSAGE(in_publics == 0, tag << ": " << in_publics << " STATIC anchors are publics in the PDB");
            CHECK_MESSAGE(in_module == 0, tag << ": " << in_module << " STATIC anchors are module symbols in the PDB");
            evidence("set=%s linker=%s | static-anchor probe: in PDB publics %zu, in module symbols %zu", name.c_str(), lk.label.c_str(), in_publics,
                     in_module);
            spdb.index.reset();
            std::error_code ec;
            fs::remove(ls.dll, ec);
            fs::remove(ls.pdb, ec);
        }
        pdb.index.reset();
        std::error_code ec;
        fs::remove(la.dll, ec);
        fs::remove(la.pdb, ec);
        fs::remove(lp.dll, ec);
        fs::remove(lp.pdb, ec);
    }
}

void run_set(const SetSpec& spec) {
    std::vector<std::string> files;
    for (const auto& [key, stem] : spec.objects) {
        files.push_back(find_object(key, stem));
    }
    run_files(spec.name, files, false);
}

}  // namespace

TEST_CASE("coff link: basic/logic.obj") { run_set({"basic_logic", {{"basic_objs", "logic"}}}); }
TEST_CASE("coff link: basic/extras.obj (COMDATs, associative .pdata/.xdata, .CRT$XCU)") {
    run_set({"basic_extras", {{"basic_objs", "extras"}}});
}
TEST_CASE("coff link: basic/main.obj") { run_set({"basic_main", {{"basic_objs", "main"}}}); }
TEST_CASE("coff link: basic main+logic+extras together (shared COMDATs)") {
    run_set({"basic_all", {{"basic_objs", "main"}, {"basic_objs", "logic"}, {"basic_objs", "extras"}}});
}
TEST_CASE("coff link: extras.cpp compiled /Od (every construct present)") {
    run_set({"cov_extras", {{"cov_extras_objs", "extras"}}});
}
TEST_CASE("coff link: bigobj fixture (bigobj_funcs + main)") {
    run_set({"bigobj", {{"bigobj_objs", "bigobj_funcs"}, {"bigobj_objs", "main"}}});
}

// Two objects that both define the COMDATs shared_fn and shared_const: the linker keeps one copy and
// drops the other. Every locator must still resolve (both objects' locators to the same address) and
// match the PDB's section contributions; STATIC anchors are probed as invisible.
TEST_CASE("coff link: two objects sharing a COMDAT, locators under both linkers") {
    const fs::path dir = work_root() / "synthetic_inputs";
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }
    std::vector<std::string> files;
    for (const char* which : {"a", "b"}) {
        const std::string path = (dir / (std::string("shared_") + which + ".obj")).string();
        REQUIRE(write_file(path, make_shared_comdat_object(which)));
        files.push_back(path);
    }
    run_files("shared_comdat", files, true);
}
