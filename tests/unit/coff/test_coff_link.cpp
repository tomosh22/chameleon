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
    const auto bytes_of = [](const cml::pe::OwnedImage& img, const cml::pe::SectionHeader& s) {
        const std::size_t off = s.pointer_to_raw_data;
        const std::size_t len = s.size_of_raw_data;
        if (off > img.bytes.size() || len > img.bytes.size() - off) {
            return std::vector<std::byte>();
        }
        return std::vector<std::byte>(img.bytes.begin() + static_cast<std::ptrdiff_t>(off),
                                      img.bytes.begin() + static_cast<std::ptrdiff_t>(off + len));
    };
    for (std::size_t i = 0; i < sa.size(); ++i) {
        if (sa[i].raw_name != sb[i].raw_name || sa[i].virtual_address != sb[i].virtual_address ||
            sa[i].virtual_size != sb[i].virtual_size || sa[i].size_of_raw_data != sb[i].size_of_raw_data ||
            sa[i].characteristics != sb[i].characteristics) {
            d.layout_equal = false;
            continue;
        }
        const bool same = bytes_of(*a, sa[i]) == bytes_of(*b, sb[i]);
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

// ---------------------------------------------------------------------------------------------
// One object set
// ---------------------------------------------------------------------------------------------

struct SetSpec {
    std::string name;
    std::vector<std::pair<const char*, std::string>> objects;  // {fixture list key, file stem}
    bool single_object() const { return objects.size() == 1; }
};

struct Variant {
    std::vector<std::string> files;                   // object files to link (binding.obj last)
    std::vector<std::vector<coff::Anchor>> anchors;   // per object (empty for the plain variant)
};

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

void run_set(const SetSpec& spec) {
    const std::vector<Linker> linkers = linkers_under_test();
    const fs::path dir = work_root() / spec.name;
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
    }

    // ---- objects, bind list, binding object ----
    std::vector<std::string> originals;
    std::vector<coff::OwnedObject> objects;
    for (const auto& [key, stem] : spec.objects) {
        originals.push_back(find_object(key, stem));
        auto o = coff::load_object_file(originals.back());
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

    // ---- variants ----
    Variant plain;
    Variant statics;
    Variant externals;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        plain.files.push_back(originals[i]);
        plain.anchors.emplace_back();
        for (bool external : {false, true}) {
            auto m = coff::ObjectModel::from_object(objects[i].object);
            REQUIRE_MESSAGE(m.has_value(), m.error().format());
            coff::AnchorOptions opt;
            opt.external = external;
            if (external) {
                // tag left empty: the content-derived default is used
                // lld-link rejects external anchors inside COMDATs that are discarded as duplicates.
                opt.comdat_static = objects.size() > 1;
            }
            auto anchors = coff::add_anchors(*m, opt);
            REQUIRE_MESSAGE(anchors.has_value(), anchors.error().format());
            auto bytes = coff::write(*m);
            REQUIRE_MESSAGE(bytes.has_value(), bytes.error().format());
            const std::string file = (dir / (stem_of(originals[i]) + (external ? "_ext" : "_static") + ".obj")).string();
            REQUIRE(write_file(file, *bytes));
            (external ? externals : statics).files.push_back(file);
            (external ? externals : statics).anchors.push_back(std::move(*anchors));
        }
    }
    for (Variant* v : {&plain, &statics, &externals}) {
        v->files.push_back(binding_path.string());
    }

    for (const Linker& lk : linkers) {
        INFO("set " << spec.name << ", linker " << lk.label);
        const std::string tag = spec.name + "/" + lk.label;

        // 1. plain link with the binding object: must succeed, thunks and slots must be right.
        const fs::path ldir = dir / lk.label;
        const Linked lp = do_link(lk, ldir / "p0", plain.files);
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

        // 2. + 3. anchored variants.
        for (bool external : {false, true}) {
            const Variant& v = external ? externals : statics;
            const char* vname = external ? "external" : "static";
            const Linked la = do_link(lk, ldir / (external ? "e0" : "s0"), v.files);
            CHECK_MESSAGE(la.ok, vname << " anchored link failed:\n" << la.output);
            REQUIRE(la.ok);
            const SectionDiff diff = compare_images(lp.dll, la.dll);
            REQUIRE_MESSAGE(diff.ok, diff.error);
            CHECK_MESSAGE(diff.layout_equal, tag << " " << vname << ": section table (names, RVAs, sizes, characteristics) differs from the unanchored link");
            CHECK_MESSAGE(diff.code_equal, tag << " " << vname << ": a code section differs from the unanchored link");

            // Where do the anchors show up?
            PdbFacts pdb = read_pdb(la.pdb);
            REQUIRE_MESSAGE(pdb.ok, pdb.error);
            std::size_t expected = 0;
            std::size_t in_publics = 0;
            std::size_t in_module = 0;
            std::size_t contrib_total = 0;
            std::size_t contrib_match = 0;
            std::size_t contrib_total_nopdata = 0;  // same, without .pdata (link.exe records no contribution for it)
            std::size_t contrib_match_nopdata = 0;
            std::size_t comdat_missing = 0;
            std::size_t plain_missing = 0;
            std::unordered_map<std::uint64_t, std::uint32_t> contribs;  // (compiland << 32 | rva) -> size
            std::vector<cml::pdb::SectionContribution> sorted_contribs;
            std::size_t unmatched_shown = 0;
            if (external) {
                sorted_contribs = pdb.index->contributions();  // sorted by rva
                contribs.reserve(sorted_contribs.size());
                for (const cml::pdb::SectionContribution& c : sorted_contribs) {
                    contribs.emplace((static_cast<std::uint64_t>(c.compiland) << 32) | c.rva, c.size);
                }
            }
            for (std::size_t i = 0; i < v.anchors.size(); ++i) {
                expected += v.anchors[i].size();
                const std::string obj_base = basename_of(v.files[i]);
                if (!external) {
                    in_module += count_module_anchors(pdb, obj_base);
                    for (const coff::Anchor& a : v.anchors[i]) {
                        in_publics += pdb.public_rva.count(a.symbol_name);
                    }
                    continue;
                }
                const auto compilands = pdb.index->compilands_by_object_name(obj_base);
                for (const coff::Anchor& a : v.anchors[i]) {
                    const auto it = pdb.public_rva.find(a.symbol_name);
                    const coff::Section* sec = objects[i].object.section(a.section_index);
                    if (it == pdb.public_rva.end()) {
                        if (sec != nullptr && sec->is_comdat()) {
                            ++comdat_missing;
                        } else {
                            ++plain_missing;
                        }
                        continue;
                    }
                    ++in_publics;
                    if (sec != nullptr && sec->size_of_raw_data != 0 && !compilands.empty()) {
                        ++contrib_total;
                        const bool is_pdata = sec->name == ".pdata";
                        contrib_total_nopdata += is_pdata ? 0 : 1;
                        const auto c = contribs.find((static_cast<std::uint64_t>(compilands.front()) << 32) | it->second);
                        if (c != contribs.end() && c->second == sec->size_of_raw_data) {
                            ++contrib_match;
                            contrib_match_nopdata += is_pdata ? 0 : 1;
                        } else if (unmatched_shown < 6) {
                            ++unmatched_shown;
                            // What the PDB has around that address, to see why the contribution does not match.
                            std::string near_text = "no contribution covers the rva";
                            auto up = std::upper_bound(sorted_contribs.begin(), sorted_contribs.end(), it->second,
                                                       [](std::uint32_t rva, const cml::pdb::SectionContribution& x) { return rva < x.rva; });
                            if (up != sorted_contribs.begin()) {
                                const cml::pdb::SectionContribution& x = *(up - 1);
                                near_text = "contribution rva=" + std::to_string(x.rva) + " size=" + std::to_string(x.size) +
                                            " compiland=" + std::to_string(x.compiland);
                            }
                            evidence("UNMATCHED set=%s linker=%s section=%u '%s' comdat=%d raw_size=%u anchor_rva=%u expected_compiland=%u | %s",
                                     spec.name.c_str(), lk.label.c_str(), static_cast<unsigned>(a.section_index),
                                     std::string(sec->name).c_str(), sec->is_comdat() ? 1 : 0, static_cast<unsigned>(sec->size_of_raw_data),
                                     static_cast<unsigned>(it->second), static_cast<unsigned>(compilands.front()), near_text.c_str());
                        }
                    }
                }
            }
            if (external) {
                // Every anchor of a non-COMDAT section must be published. A COMDAT anchor may be
                // missing only when the section was dropped as a duplicate of another object's.
                CHECK_MESSAGE(plain_missing == 0, tag << ": " << plain_missing << " anchors of ordinary sections are not publics in the PDB");
                if (spec.single_object()) {
                    CHECK_MESSAGE(comdat_missing == 0, tag << ": " << comdat_missing << " anchors of COMDAT sections are not publics in the PDB");
                }
                CHECK(in_publics > 0);
            }
            std::string joined;
            for (const std::string& n : diff.data_diffs) {
                joined += (joined.empty() ? "" : ",") + n;
            }
            evidence("set=%s linker=%s anchors=%s | link ok, bound=%zu (%zu functions) | layout_identical=%s code_identical=%s whole_image_identical=%s "
                     "differing_data_sections=[%s] outside_debug_info_bytes_differing=%zu(first in %s) | anchors_expected=%zu in_pdb_publics=%zu in_pdb_module_symbols=%zu comdat_anchors_missing=%zu "
                     "other_anchors_missing=%zu section_contributions_matching_anchor_rva=%zu/%zu (without .pdata %zu/%zu)",
                     spec.name.c_str(), lk.label.c_str(), vname, bind.size(), bind_functions, diff.layout_equal ? "yes" : "NO",
                     diff.code_equal ? "yes" : "NO", diff.whole_equal ? "yes" : "no", joined.c_str(),
                     diff.bytes_differing_outside_debug_info, diff.first_difference_in.c_str(), expected, in_publics, in_module, comdat_missing, plain_missing, contrib_match, contrib_total, contrib_match_nopdata, contrib_total_nopdata);
            pdb.index.reset();
            std::error_code ec;
            fs::remove(la.dll, ec);
            fs::remove(la.pdb, ec);
        }
        std::error_code ec;
        fs::remove(lp.dll, ec);
        fs::remove(lp.pdb, ec);
    }
}

}  // namespace

TEST_CASE("coff link: basic/logic.obj") { run_set({"basic_logic", {{"basic_objs", "logic"}}}); }
TEST_CASE("coff link: basic/extras.obj (COMDATs, associative .pdata/.xdata, .CRT$XCU)") {
    run_set({"basic_extras", {{"basic_objs", "extras"}}});
}
TEST_CASE("coff link: basic/main.obj") { run_set({"basic_main", {{"basic_objs", "main"}}}); }
TEST_CASE("coff link: basic main+logic+extras together") {
    run_set({"basic_all", {{"basic_objs", "main"}, {"basic_objs", "logic"}, {"basic_objs", "extras"}}});
}
TEST_CASE("coff link: extras.cpp compiled /Od (every construct present)") {
    run_set({"cov_extras", {{"cov_extras_objs", "extras"}}});
}
TEST_CASE("coff link: bigobj fixture (bigobj_funcs + main)") {
    run_set({"bigobj", {{"bigobj_objs", "bigobj_funcs"}, {"bigobj_objs", "main"}}});
}
