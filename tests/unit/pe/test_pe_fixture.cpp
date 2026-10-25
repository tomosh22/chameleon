// Acceptance tests: cml_pe on the executables the toolchain links from the fixtures, and (on
// Windows) on live modules mapped into this very process. Paths come from the file written by
// CMake (see tests/unit/coff/CMakeLists.txt).
#include "cml/pe/pe.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "fixture_paths.h"

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace cml::pe;
using cml_test::FixturePaths;

namespace {

const OwnedImage& load_exe(const char* key) {
    static std::map<std::string, OwnedImage> cache;
    const std::string path = FixturePaths::get().value(key);
    REQUIRE_MESSAGE(!path.empty(), "no path for '" << key << "' in " << CML_TEST_PATHS_FILE);
    auto it = cache.find(path);
    if (it == cache.end()) {
        auto r = load_image_file(path);
        REQUIRE_MESSAGE(r.has_value(), r.error().format());
        it = cache.emplace(path, std::move(r).value()).first;
    }
    return it->second;
}

bool section_is_executable(const SectionHeader& s) {
    return (s.characteristics & 0x20000000u) != 0;  // IMAGE_SCN_MEM_EXECUTE
}

std::uint32_t read_u32_at(const OwnedImage& o, std::uint64_t offset) {
    REQUIRE(offset + 4 <= o.bytes.size());
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(o.bytes[static_cast<std::size_t>(offset) + i])) << (8 * i);
    }
    return v;
}

std::uint64_t read_u64_at(const OwnedImage& o, std::uint64_t offset) {
    return static_cast<std::uint64_t>(read_u32_at(o, offset)) | (static_cast<std::uint64_t>(read_u32_at(o, offset + 4)) << 32);
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// Describes the first difference between the on-disk and the in-memory view of one module.
std::string compare_disk_and_memory(const Image& disk, const Image& mem) {
    if (disk.file_header().machine != mem.file_header().machine) return "machine";
    if (disk.file_header().time_date_stamp != mem.file_header().time_date_stamp) return "timestamp";
    if (disk.optional_header().size_of_image != mem.optional_header().size_of_image) return "size of image";
    if (disk.optional_header().address_of_entry_point != mem.optional_header().address_of_entry_point) return "entry point";
    if (disk.optional_header().dll_characteristics != mem.optional_header().dll_characteristics) return "dll characteristics";
    if (disk.sections().size() != mem.sections().size()) return "section count";
    for (std::size_t i = 0; i < disk.sections().size(); ++i) {
        if (disk.sections()[i].name() != mem.sections()[i].name()) return "section name";
        if (disk.sections()[i].virtual_address != mem.sections()[i].virtual_address) return "section rva";
    }

    const auto di = disk.imports();
    const auto mi = mem.imports();
    if (!di) return "disk imports: " + di.error().format();
    if (!mi) return "memory imports: " + mi.error().format();
    if (di->size() != mi->size()) return "import module count";
    for (std::size_t m = 0; m < di->size(); ++m) {
        const ImportModule& a = (*di)[m];
        const ImportModule& b = (*mi)[m];
        if (a.dll_name != b.dll_name) return "import dll name";
        if (a.iat_rva != b.iat_rva || a.lookup_table_rva != b.lookup_table_rva) return "import table rvas";
        if (a.entries.size() != b.entries.size()) return "import entry count for " + std::string(a.dll_name);
        for (std::size_t e = 0; e < a.entries.size(); ++e) {
            if (a.entries[e].name != b.entries[e].name || a.entries[e].by_ordinal != b.entries[e].by_ordinal ||
                a.entries[e].ordinal != b.entries[e].ordinal || a.entries[e].iat_slot_rva != b.entries[e].iat_slot_rva) {
                return "import entry " + std::string(a.dll_name) + "#" + std::to_string(e);
            }
        }
    }

    const auto dc = disk.codeview();
    const auto mc = mem.codeview();
    if (!dc) return "disk codeview: " + dc.error().format();
    if (!mc) return "memory codeview: " + mc.error().format();
    if (dc->present != mc->present) return "codeview presence";
    if (dc->guid.to_string() != mc->guid.to_string() || dc->age != mc->age || dc->pdb_path != mc->pdb_path) return "codeview contents";

    const auto de = disk.exception_table();
    const auto me = mem.exception_table();
    if (!de) return "disk exceptions: " + de.error().format();
    if (!me) return "memory exceptions: " + me.error().format();
    if (de->entries.size() != me->entries.size()) return "exception entry count";
    for (std::size_t i = 0; i < de->entries.size(); ++i) {
        if (de->entries[i].begin_rva != me->entries[i].begin_rva || de->entries[i].end_rva != me->entries[i].end_rva ||
            de->entries[i].unwind_rva != me->entries[i].unwind_rva) {
            return "exception entry " + std::to_string(i);
        }
    }

    const auto dl = disk.load_config();
    const auto ml = mem.load_config();
    if (!dl) return "disk load config: " + dl.error().format();
    if (!ml) return "memory load config: " + ml.error().format();
    if (dl->present != ml->present || dl->guard_flags != ml->guard_flags || dl->has_guard_flags != ml->has_guard_flags) {
        return "load config guard flags";
    }
    if (disk.has_tls() != mem.has_tls()) return "tls presence";
    return std::string();
}

}  // namespace

TEST_CASE("fixture basic exe: headers and sections") {
    const OwnedImage& o = load_exe("basic_exe");
    const Image& img = o.image;
    CHECK(img.kind() == ImageKind::OnDisk);
    CHECK(img.file_header().machine == 0x8664u);
    CHECK(img.optional_header().magic == 0x20Bu);
    CHECK(img.optional_header().subsystem == 3u);  // IMAGE_SUBSYSTEM_WINDOWS_CUI
    CHECK(img.optional_header().image_base != 0u);
    CHECK(img.optional_header().section_alignment >= img.optional_header().file_alignment);
    CHECK(img.optional_header().size_of_image > img.optional_header().size_of_headers);

    for (const char* name : {".text", ".rdata", ".data", ".pdata"}) {
        CHECK_MESSAGE(img.find_section(name) != nullptr, "missing section " << name);
    }
    const SectionHeader* entry_section = img.section_for_rva(img.optional_header().address_of_entry_point);
    REQUIRE(entry_section != nullptr);
    CHECK(section_is_executable(*entry_section));

    for (const SectionHeader& s : img.sections()) {
        CHECK_MESSAGE(static_cast<std::uint64_t>(s.virtual_address) + s.virtual_size <= img.optional_header().size_of_image,
                      "section " << s.name() << " leaves the image");
        // File-backed bytes of a section: min(VirtualSize, SizeOfRawData) (VirtualSize 0 means raw size).
        const std::uint32_t backed = std::min<std::uint32_t>(s.virtual_size != 0 ? s.virtual_size : s.size_of_raw_data, s.size_of_raw_data);
        if (backed != 0) {
            // Mapping round trip for the first and last byte of every section's file data.
            CHECK(img.rva_to_offset(s.virtual_address) == s.pointer_to_raw_data);
            CHECK(img.offset_to_rva(s.pointer_to_raw_data) == s.virtual_address);
            const std::uint64_t last = static_cast<std::uint64_t>(s.pointer_to_raw_data) + backed - 1;
            if (last < o.bytes.size()) {
                CHECK(img.offset_to_rva(last) == s.virtual_address + backed - 1);
                CHECK(img.rva_to_offset(s.virtual_address + backed - 1) == last);
            }
        }
    }
}

TEST_CASE("fixture basic exe: imports GetTickCount64 from KERNEL32.dll with a valid IAT slot") {
    const OwnedImage& o = load_exe("basic_exe");
    const Image& img = o.image;
    const auto imports = img.imports();
    REQUIRE_MESSAGE(imports.has_value(), imports.error().format());
    REQUIRE_FALSE(imports->empty());

    const ImportEntry* tick = find_import(*imports, "KERNEL32.dll", "GetTickCount64");
    REQUIRE_MESSAGE(tick != nullptr, "GetTickCount64 is not imported from KERNEL32.dll");
    CHECK_FALSE(tick->by_ordinal);

    // The slot lies inside the IAT data directory and in a writable-or-readonly data section.
    const DataDirectory& iat = img.data_directory(dir::Iat);
    CHECK(iat.rva != 0u);
    CHECK(tick->iat_slot_rva >= iat.rva);
    CHECK(static_cast<std::uint64_t>(tick->iat_slot_rva) + 8 <= static_cast<std::uint64_t>(iat.rva) + iat.size);
    CHECK(tick->iat_slot_rva % 8u == 0u);
    REQUIRE(img.section_for_rva(tick->iat_slot_rva) != nullptr);

    // Find the module and entry index, then check the on-disk slot holds the same unbound thunk
    // as the lookup table (the loader overwrites it with the address at load time).
    bool verified = false;
    for (const ImportModule& m : *imports) {
        for (std::size_t i = 0; i < m.entries.size(); ++i) {
            if (&m.entries[i] != tick) {
                continue;
            }
            CHECK(m.iat_rva + 8u * i == tick->iat_slot_rva);
            const auto slot = img.read_rva(tick->iat_slot_rva, 8);
            REQUIRE(slot.has_value());
            if (m.lookup_table_rva != 0) {
                const auto thunk = img.read_rva(m.lookup_table_rva + 8u * static_cast<std::uint32_t>(i), 8);
                REQUIRE(thunk.has_value());
                CHECK(std::equal(slot->begin(), slot->end(), thunk->begin()));
            }
            verified = true;
        }
    }
    CHECK(verified);

    // The fixture's other direct Win32 calls.
    CHECK(find_import(*imports, "KERNEL32.dll", "Sleep") != nullptr);
    CHECK(find_import(*imports, "KERNEL32.dll", "GetCurrentProcessId") != nullptr);

    // Every named entry has a name, every module has a name; hints fit 16 bits trivially.
    for (const ImportModule& m : *imports) {
        CHECK_FALSE(m.dll_name.empty());
        CHECK(m.names_available);
        for (const ImportEntry& e : m.entries) {
            CHECK((e.by_ordinal || !e.name.empty()));
        }
    }
}

TEST_CASE("fixture basic exe: debug directory points at the PDB") {
    const OwnedImage& o = load_exe("basic_exe");
    const auto entries = o.image.debug_entries();
    REQUIRE_MESSAGE(entries.has_value(), entries.error().format());
    CHECK_FALSE(entries->empty());

    const auto cv = o.image.codeview();
    REQUIRE_MESSAGE(cv.has_value(), cv.error().format());
    REQUIRE(cv->present);
    CHECK_FALSE(cv->guid.is_zero());
    CHECK_MESSAGE(cv->guid.to_string().size() == 36u, cv->guid.to_string());
    CHECK(cv->age >= 1u);
    CHECK_FALSE(cv->pdb_path.empty());

    const std::string expected = FixturePaths::get().value("basic_pdb");
    REQUIRE_FALSE(expected.empty());
    CHECK_MESSAGE(lower(FixturePaths::basename(std::string(cv->pdb_path))) == lower(FixturePaths::basename(expected)),
                  "CodeView path '" << cv->pdb_path << "' vs build output '" << expected << "'");
    CHECK_MESSAGE(std::ifstream(expected, std::ios::binary).good(), "PDB does not exist: " << expected);
}

TEST_CASE("fixture basic exe: exception directory is non-empty and sorted") {
    const OwnedImage& o = load_exe("basic_exe");
    const Image& img = o.image;
    const auto table = img.exception_table();
    REQUIRE_MESSAGE(table.has_value(), table.error().format());
    REQUIRE(table->entries.size() > 5u);
    CHECK(table->sorted);

    std::uint32_t previous_end = 0;
    for (std::size_t i = 0; i < table->entries.size(); ++i) {
        const RuntimeFunction& f = table->entries[i];
        INFO("entry " << i);
        REQUIRE(f.begin_rva < f.end_rva);
        CHECK(f.begin_rva >= previous_end);  // functions do not overlap
        CHECK(f.unwind_rva != 0u);
        previous_end = f.end_rva;
        const SectionHeader* s = img.section_for_rva(f.begin_rva);
        REQUIRE(s != nullptr);
        CHECK(section_is_executable(*s));
        CHECK(table->find(f.begin_rva) == &f);
        CHECK(table->find(f.end_rva - 1) == &f);
        CHECK(img.read_rva(f.unwind_rva & ~1u, 4).has_value());
    }
    CHECK(table->find(0) == nullptr);
    CHECK(table->find(0xFFFFFFFFu) == nullptr);
}

TEST_CASE("fixture basic exe: load config GuardFlags and TLS directory") {
    const OwnedImage& o = load_exe("basic_exe");
    const Image& img = o.image;
    const auto lc = img.load_config();
    REQUIRE_MESSAGE(lc.has_value(), lc.error().format());
    REQUIRE(lc->present);
    REQUIRE(lc->has_guard_flags);

    // Independent read of GuardFlags (offset 0x90 of IMAGE_LOAD_CONFIG_DIRECTORY64) from the raw file.
    const DataDirectory& dd = img.data_directory(dir::LoadConfig);
    const auto off = img.rva_to_offset(dd.rva);
    REQUIRE(off.has_value());
    CHECK(read_u32_at(o, *off) == lc->size);
    CHECK(read_u32_at(o, *off + 0x90) == lc->guard_flags);
    CHECK(read_u64_at(o, *off + 0x58) == lc->security_cookie);

    // The linker sets CF_INSTRUMENTED in GuardFlags whenever the CRT objects it links were compiled
    // with /guard:cf, even without /guard:cf on the fixture itself; the DllCharacteristics GuardCf bit
    // is only set by /guard:cf at link time. So the two are NOT required to agree; what must hold is
    // that GuardFlags is read from the right place (checked against the raw file above) and that
    // the function table flag implies a table.
    if ((lc->guard_flags & guard::CfFunctionTablePresent) != 0) {
        CHECK(lc->guard_cf_function_table != 0u);
    }
    CHECK(lc->security_cookie != 0u);  // /GS is on, so the cookie slot exists

    const DataDirectory& tls_dir = img.data_directory(dir::Tls);
    CHECK(img.has_tls() == (tls_dir.rva != 0 && tls_dir.size != 0));
    const auto tls = img.tls();
    REQUIRE_MESSAGE(tls.has_value(), tls.error().format());
    CHECK(tls->present == img.has_tls());
    if (tls->present) {
        const std::uint64_t lo = img.optional_header().image_base;
        const std::uint64_t hi = lo + img.optional_header().size_of_image;
        CHECK(tls->address_of_index >= lo);
        CHECK(tls->address_of_index < hi);
        CHECK(tls->start_address_of_raw_data <= tls->end_address_of_raw_data);
    }

    const auto ex = img.exports();
    REQUIRE_MESSAGE(ex.has_value(), ex.error().format());  // an exe normally exports nothing
}

TEST_CASE("fixture bigobj exe: a large exception table is sorted and searchable") {
    const OwnedImage& o = load_exe("bigobj_exe");
    const auto table = o.image.exception_table();
    REQUIRE_MESSAGE(table.has_value(), table.error().format());
    CHECK(table->entries.size() > 5u);
    CHECK(table->sorted);
    for (const RuntimeFunction& f : table->entries) {
        REQUIRE(table->find(f.begin_rva) == &f);
    }
}

TEST_CASE("fixture basic exe: a copy laid out like a loaded image parses identically in memory") {
    // Reproduce the loader's mapping by hand (headers, then each section's raw data at its RVA)
    // so the in-memory variant is tested against real linker output on every platform.
    const OwnedImage& o = load_exe("basic_exe");
    const Image& disk = o.image;
    std::vector<std::byte> mapped(disk.optional_header().size_of_image);
    const std::size_t header_bytes = std::min<std::size_t>(disk.optional_header().size_of_headers, o.bytes.size());
    std::copy(o.bytes.begin(), o.bytes.begin() + static_cast<std::ptrdiff_t>(header_bytes), mapped.begin());
    for (const SectionHeader& s : disk.sections()) {
        if (s.size_of_raw_data == 0 || s.pointer_to_raw_data >= o.bytes.size()) {
            continue;
        }
        const std::size_t n = std::min<std::size_t>({s.size_of_raw_data, s.virtual_size == 0 ? s.size_of_raw_data : s.virtual_size,
                                                     o.bytes.size() - s.pointer_to_raw_data});
        REQUIRE(static_cast<std::uint64_t>(s.virtual_address) + n <= mapped.size());
        std::copy_n(o.bytes.begin() + s.pointer_to_raw_data, n, mapped.begin() + s.virtual_address);
    }
    const auto mem = Image::parse(ByteSpan(mapped.data(), mapped.size()), ImageKind::InMemory);
    REQUIRE_MESSAGE(mem.has_value(), mem.error().format());
    const std::string diff = compare_disk_and_memory(disk, *mem);
    CHECK_MESSAGE(diff.empty(), diff);

    const auto base = Image::parse_mapped_module(mapped.data());
    REQUIRE_MESSAGE(base.has_value(), base.error().format());
    CHECK(base->bytes().size() == mapped.size());
    CHECK(compare_disk_and_memory(disk, *base).empty());
}

#if defined(_WIN32)

namespace {

std::string module_path(HMODULE module) {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(module, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    REQUIRE(n > 0);
    std::string out;
    for (DWORD i = 0; i < n; ++i) {
        out.push_back(buf[i] < 0x80 ? static_cast<char>(buf[i]) : '?');
    }
    return out;
}

}  // namespace

TEST_CASE("in-memory: this test executable matches its own file on disk") {
    HMODULE self = GetModuleHandleW(nullptr);
    REQUIRE(self != nullptr);
    const auto mem = Image::parse_mapped_module(self);
    REQUIRE_MESSAGE(mem.has_value(), mem.error().format());
    CHECK(mem->kind() == ImageKind::InMemory);
    CHECK(mem->bytes().data() == reinterpret_cast<const std::byte*>(self));
    CHECK(mem->bytes().size() == static_cast<std::size_t>(mem->optional_header().size_of_image));

    const auto disk = load_image_file(module_path(self));
    REQUIRE_MESSAGE(disk.has_value(), disk.error().format());
    const std::string diff = compare_disk_and_memory(disk->image, *mem);
    CHECK_MESSAGE(diff.empty(), diff);

    // In memory RVA == offset, and the entry point is real code.
    CHECK(mem->rva_to_offset(mem->optional_header().address_of_entry_point) == mem->optional_header().address_of_entry_point);
    const SectionHeader* entry = mem->section_for_rva(mem->optional_header().address_of_entry_point);
    REQUIRE(entry != nullptr);
    CHECK(section_is_executable(*entry));
    CHECK(mem->optional_header().image_base != 0u);  // the preferred base; ASLR may have moved the module

    // Imports of the running test exe: it calls into KERNEL32 for sure.
    const auto imports = mem->imports();
    REQUIRE_MESSAGE(imports.has_value(), imports.error().format());
    bool kernel32 = false;
    for (const ImportModule& m : *imports) {
        kernel32 = kernel32 || lower(m.dll_name) == "kernel32.dll";
    }
    CHECK(kernel32);

    // The loaded IAT slot of an import must hold the address GetProcAddress reports.
    const ImportEntry* ge = find_import(*imports, "kernel32.dll", "GetModuleHandleW");
    if (ge != nullptr) {
        const std::uint64_t slot_value = *reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::byte*>(self) + ge->iat_slot_rva);
        const void* expected = reinterpret_cast<const void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetModuleHandleW"));
        CHECK(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(slot_value)) == expected);
    }
}

TEST_CASE("in-memory: KERNEL32 exports agree with GetProcAddress, including forwarders") {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    REQUIRE(k32 != nullptr);
    const auto img = Image::parse_mapped_module(k32);
    REQUIRE_MESSAGE(img.has_value(), img.error().format());
    const auto ex = img->exports();
    REQUIRE_MESSAGE(ex.has_value(), ex.error().format());
    REQUIRE(ex->present);
    CHECK(lower(ex->dll_name) == "kernel32.dll");
    CHECK(ex->entries.size() > 500u);

    const auto* base = reinterpret_cast<const std::byte*>(k32);
    std::size_t checked = 0;
    std::size_t forwarders = 0;
    std::size_t by_ordinal_checked = 0;
    for (const Export& e : ex->entries) {
        if (e.rva == 0) {
            continue;
        }
        if (e.is_forwarder) {
            ++forwarders;
            CHECK_MESSAGE(e.forwarder.find('.') != std::string_view::npos, "forwarder " << e.forwarder);
            continue;
        }
        if (!e.has_name) {
            continue;
        }
        const std::string name(e.name);
        const FARPROC p = GetProcAddress(k32, name.c_str());
        REQUIRE_MESSAGE(p != nullptr, name);
        CHECK_MESSAGE(reinterpret_cast<const void*>(p) == static_cast<const void*>(base + e.rva), name);
        ++checked;
        if (by_ordinal_checked < 25) {
            const FARPROC q = GetProcAddress(k32, MAKEINTRESOURCEA(e.ordinal));
            CHECK_MESSAGE(reinterpret_cast<const void*>(q) == reinterpret_cast<const void*>(p), name << " ordinal " << e.ordinal);
            ++by_ordinal_checked;
        }
    }
    CHECK(checked > 300u);
    CHECK_MESSAGE(forwarders > 0u, "kernel32 normally forwards many exports to NTDLL");

    const Export* tick = ex->find_by_name("GetTickCount64");
    REQUIRE(tick != nullptr);
    if (tick->is_forwarder) {
        CHECK(tick->forwarder.find("GetTickCount64") != std::string_view::npos);
    } else {
        CHECK(reinterpret_cast<const void*>(GetProcAddress(k32, "GetTickCount64")) == static_cast<const void*>(base + tick->rva));
    }
    CHECK(ex->find_by_ordinal(tick->ordinal) == tick);
}

TEST_CASE("in-memory: the fixture exe mapped as an image resource matches its on-disk parse") {
    const OwnedImage& o = load_exe("basic_exe");
    const std::string path = FixturePaths::get().value("basic_exe");
    std::wstring wide(path.begin(), path.end());
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    HMODULE mapped = LoadLibraryExW(wide.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    REQUIRE_MESSAGE(mapped != nullptr, "LoadLibraryExW failed, error " << GetLastError());
    // The low bits of an image-resource handle are flags, not address bits.
    const void* base = reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(mapped) & ~static_cast<std::uintptr_t>(3));
    const auto mem = Image::parse_mapped_module(base);
    REQUIRE_MESSAGE(mem.has_value(), mem.error().format());
    const std::string diff = compare_disk_and_memory(o.image, *mem);
    CHECK_MESSAGE(diff.empty(), diff);

    const auto imports = mem->imports();
    REQUIRE(imports.has_value());
    CHECK(find_import(*imports, "KERNEL32.dll", "GetTickCount64") != nullptr);
    const auto cv = mem->codeview();
    REQUIRE(cv.has_value());
    CHECK(cv->present);
    CHECK_FALSE(cv->guid.is_zero());
    FreeLibrary(mapped);
}

#endif  // _WIN32

