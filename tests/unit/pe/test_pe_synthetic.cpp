// cml_pe on a hand-built PE32+ image, in both the on-disk and the in-memory layout, plus malformed
// input. The builder (pe_builder.h) shares no code with the library.
#include "cml/pe/pe.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "doctest.h"
#include "pe_builder.h"

using namespace cml::pe;
using namespace cml_test;
using namespace cml_test::pe_layout;

namespace {

Image parse_ok(const PeBytes& bytes, ImageKind kind) {
    auto r = Image::parse(pe_span(bytes), kind);
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    return std::move(r).value();
}

void expect_parse_error(const PeBytes& bytes, ImageKind kind, std::uint32_t code) {
    const auto r = Image::parse(pe_span(bytes), kind);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == code);
    CHECK_FALSE(r.error().message.empty());
    CHECK_FALSE(r.error().what_to_do.empty());
}

// Runs every directory reader and mapping helper; used on mutated images.
void exercise(const Image& img) {
    (void)img.exports();
    (void)img.imports();
    (void)img.debug_entries();
    (void)img.codeview();
    (void)img.exception_table();
    (void)img.load_config();
    (void)img.tls();
    (void)img.has_tls();
    for (std::uint32_t rva = 0; rva < 0x5000; rva += 0x1D3) {
        (void)img.rva_to_offset(rva);
        (void)img.read_cstring_rva(rva);
        (void)img.read_rva(rva, 16);
    }
    (void)img.read_rva(0xFFFFFFFFu, 0xFFFFFFFFFFull);
    (void)img.offset_to_rva(0xFFFFFFFFull);
}

void check_directories(const Image& img) {
    // ---- exports ----
    const auto ex = img.exports();
    REQUIRE_MESSAGE(ex.has_value(), ex.error().format());
    CHECK(ex->present);
    CHECK(ex->dll_name == "test.dll");
    CHECK(ex->ordinal_base == 10u);
    REQUIRE(ex->entries.size() == 3u);
    CHECK(ex->entries[0].ordinal == 10u);
    CHECK(ex->entries[0].rva == kTextRva);
    CHECK(ex->entries[0].has_name);
    CHECK(ex->entries[0].name == "alpha");
    CHECK_FALSE(ex->entries[0].is_forwarder);
    CHECK(ex->entries[1].ordinal == 11u);
    CHECK(ex->entries[1].rva == 0u);
    CHECK_FALSE(ex->entries[1].has_name);
    CHECK(ex->entries[2].ordinal == 12u);
    CHECK(ex->entries[2].name == "beta");
    CHECK(ex->entries[2].is_forwarder);
    CHECK(ex->entries[2].forwarder == "NTDLL.RtlFoo");
    CHECK(ex->find_by_name("beta") == &ex->entries[2]);
    CHECK(ex->find_by_name("gamma") == nullptr);
    CHECK(ex->find_by_ordinal(10) == &ex->entries[0]);
    CHECK(ex->find_by_ordinal(12) == &ex->entries[2]);
    CHECK(ex->find_by_ordinal(9) == nullptr);
    CHECK(ex->find_by_ordinal(13) == nullptr);

    // ---- imports ----
    const auto im = img.imports();
    REQUIRE_MESSAGE(im.has_value(), im.error().format());
    REQUIRE(im->size() == 1u);
    const ImportModule& k32 = (*im)[0];
    CHECK(k32.dll_name == "KERNEL32.dll");
    CHECK(k32.lookup_table_rva == kIlt);
    CHECK(k32.iat_rva == kIat);
    CHECK(k32.names_available);
    REQUIRE(k32.entries.size() == 2u);
    CHECK_FALSE(k32.entries[0].by_ordinal);
    CHECK(k32.entries[0].name == "GetTickCount64");
    CHECK(k32.entries[0].hint == 0x1F0u);
    CHECK(k32.entries[0].iat_slot_rva == kIat);
    CHECK(k32.entries[1].by_ordinal);
    CHECK(k32.entries[1].ordinal == 7u);
    CHECK(k32.entries[1].iat_slot_rva == kIat + 8u);
    const ImportEntry* found = find_import(*im, "kernel32.DLL", "GetTickCount64");
    REQUIRE(found != nullptr);
    CHECK(found->iat_slot_rva == kIat);
    CHECK(find_import(*im, "kernel32.dll", "Nope") == nullptr);
    CHECK(find_import(*im, "user32.dll", "GetTickCount64") == nullptr);

    // ---- debug / CodeView ----
    const auto dbg = img.debug_entries();
    REQUIRE_MESSAGE(dbg.has_value(), dbg.error().format());
    REQUIRE(dbg->size() == 1u);
    CHECK((*dbg)[0].type == kDebugTypeCodeView);
    const auto cv = img.codeview();
    REQUIRE_MESSAGE(cv.has_value(), cv.error().format());
    REQUIRE(cv->present);
    CHECK_FALSE(cv->guid.is_zero());
    CHECK(cv->guid.data1 == 0x04030201u);
    CHECK(cv->guid.data2 == 0x0605u);
    CHECK(cv->guid.data3 == 0x0807u);
    CHECK(cv->guid.data4[0] == 9u);
    CHECK(cv->guid.data4[7] == 16u);
    CHECK(cv->guid.to_string() == "04030201-0605-0807-090A-0B0C0D0E0F10");
    CHECK(cv->age == 3u);
    CHECK(cv->pdb_path == "C:\\build\\test.pdb");
    CHECK(cv->time_date_stamp == 0x5F001234u);

    // ---- exception directory ----
    const auto exc = img.exception_table();
    REQUIRE_MESSAGE(exc.has_value(), exc.error().format());
    REQUIRE(exc->entries.size() == 3u);
    CHECK(exc->sorted);
    CHECK(exc->entries[2].unwind_rva == 0x2821u);
    const RuntimeFunction* f = exc->find(0x1000);
    REQUIRE(f != nullptr);
    CHECK(f == &exc->entries[0]);
    CHECK(exc->find(0x100F) == &exc->entries[0]);
    CHECK(exc->find(0x1010) == &exc->entries[1]);  // end is exclusive
    CHECK(exc->find(0x103F) == &exc->entries[1]);
    CHECK(exc->find(0x1040) == nullptr);           // gap
    CHECK(exc->find(0x1100) == &exc->entries[2]);
    CHECK(exc->find(0x117F) == &exc->entries[2]);
    CHECK(exc->find(0x1180) == nullptr);
    CHECK(exc->find(0x0FFF) == nullptr);

    // ---- load config ----
    const auto lc = img.load_config();
    REQUIRE_MESSAGE(lc.has_value(), lc.error().format());
    REQUIRE(lc->present);
    CHECK(lc->size == kLoadConfigSize);
    CHECK(lc->security_cookie == 0x140003000ull);
    CHECK(lc->guard_cf_check_function_pointer == 0x140001234ull);
    CHECK(lc->guard_cf_dispatch_function_pointer == 0x140001240ull);
    CHECK(lc->guard_cf_function_table == 0x140002F00ull);
    CHECK(lc->guard_cf_function_count == 5u);
    CHECK(lc->has_guard_flags);
    CHECK(lc->guard_flags == (guard::CfInstrumented | guard::CfFunctionTablePresent));
    CHECK(lc->cf_instrumented());

    // ---- TLS ----
    CHECK(img.has_tls());
    const auto tls = img.tls();
    REQUIRE_MESSAGE(tls.has_value(), tls.error().format());
    CHECK(tls->present);
    CHECK(tls->start_address_of_raw_data == 0x140004000ull);
    CHECK(tls->end_address_of_raw_data == 0x140004100ull);
    CHECK(tls->address_of_index == 0x140003010ull);
    CHECK(tls->address_of_callbacks == 0x140002100ull);
    CHECK(tls->size_of_zero_fill == 0x20u);
}

}  // namespace

TEST_CASE("pe synthetic: headers and sections (on disk)") {
    const PeBytes disk = build_pe_on_disk();
    const Image img = parse_ok(disk, ImageKind::OnDisk);
    CHECK(img.kind() == ImageKind::OnDisk);
    CHECK(img.file_header().machine == 0x8664u);
    CHECK(img.file_header().number_of_sections == 3u);
    CHECK(img.file_header().size_of_optional_header == 0xF0u);
    CHECK(img.optional_header().magic == 0x20Bu);
    CHECK(img.optional_header().image_base == 0x140000000ull);
    CHECK(img.optional_header().address_of_entry_point == kTextRva);
    CHECK(img.optional_header().size_of_image == kImageSize);
    CHECK(img.optional_header().subsystem == 3u);
    CHECK((img.optional_header().dll_characteristics & dllchar::GuardCf) != 0u);
    CHECK(img.optional_header().number_of_rva_and_sizes == 16u);
    CHECK(img.nt_headers_offset() == kLfanew);
    REQUIRE(img.sections().size() == 3u);
    CHECK(img.sections()[0].name() == ".text");
    CHECK(img.sections()[1].name() == ".rdata");
    CHECK(img.sections()[2].name() == ".pdata");
    CHECK(img.find_section(".rdata") == &img.sections()[1]);
    CHECK(img.find_section(".nope") == nullptr);
    CHECK(img.section_for_rva(0x2345) == &img.sections()[1]);
    CHECK(img.section_for_rva(0x4000) == nullptr);
    CHECK(img.data_directory(dir::Import).rva == kImportDir);
    CHECK(img.data_directory(dir::Import).size == 0x28u);
    CHECK(img.data_directory(dir::Resource).rva == 0u);
    CHECK(img.data_directory(99).rva == 0u);
}

TEST_CASE("pe synthetic: RVA <-> file offset mapping differs between the two layouts") {
    const PeBytes disk = build_pe_on_disk();
    const Image d = parse_ok(disk, ImageKind::OnDisk);
    CHECK(d.rva_to_offset(0x10) == 0x10u);  // headers map 1:1
    CHECK(d.rva_to_offset(kTextRva) == kTextRaw);
    CHECK(d.rva_to_offset(kTextRva + 0x1FF) == kTextRaw + 0x1FFu);
    CHECK(d.rva_to_offset(kRdataRva + 0x123) == kRdataRaw + 0x123u);
    CHECK(d.rva_to_offset(kPdataRva) == kPdataRaw);
    CHECK_FALSE(d.rva_to_offset(0x0F00).has_value());           // gap between headers and .text
    CHECK(d.rva_to_offset(kPdataRva + 0x23).has_value());        // last byte of VirtualSize (0x24)
    CHECK_FALSE(d.rva_to_offset(kPdataRva + 0x24).has_value());  // raw padding past VirtualSize is never mapped
    CHECK_FALSE(d.rva_to_offset(kPdataRva + 0x40).has_value());  // past the raw data
    CHECK_FALSE(d.rva_to_offset(kImageSize).has_value());
    CHECK(d.offset_to_rva(kTextRaw) == kTextRva);
    CHECK(d.offset_to_rva(kRdataRaw + 0x10) == kRdataRva + 0x10u);
    CHECK(d.offset_to_rva(0x20) == 0x20u);
    CHECK_FALSE(d.offset_to_rva(0xF000).has_value());

    const PeBytes mem = build_pe_in_memory(disk);
    const Image m = parse_ok(mem, ImageKind::InMemory);
    CHECK(m.kind() == ImageKind::InMemory);
    CHECK(m.rva_to_offset(kRdataRva + 0x123) == kRdataRva + 0x123u);  // RVA == offset
    CHECK(m.rva_to_offset(0x0F00) == 0x0F00u);                         // gaps are readable memory
    CHECK_FALSE(m.rva_to_offset(kImageSize).has_value());
    CHECK(m.offset_to_rva(0x2345) == 0x2345u);
    CHECK_FALSE(m.offset_to_rva(kImageSize).has_value());

    // The same RVA reads the same bytes in both layouts.
    const auto a = d.read_rva(kHintName + 2, 14);
    const auto b = m.read_rva(kHintName + 2, 14);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(std::string(reinterpret_cast<const char*>(a->data()), 14) == "GetTickCount64");
    CHECK(std::string(reinterpret_cast<const char*>(b->data()), 14) == "GetTickCount64");
    CHECK(a->data() != b->data());
}

TEST_CASE("pe synthetic: directories (on disk)") {
    const PeBytes disk = build_pe_on_disk();
    check_directories(parse_ok(disk, ImageKind::OnDisk));
}

TEST_CASE("pe synthetic: directories (in memory)") {
    const PeBytes mem = build_pe_in_memory(build_pe_on_disk());
    check_directories(parse_ok(mem, ImageKind::InMemory));
}

TEST_CASE("pe synthetic: parse_mapped_module reads SizeOfImage from the headers at the base") {
    const PeBytes mem = build_pe_in_memory(build_pe_on_disk());
    const auto r = Image::parse_mapped_module(mem.data());
    REQUIRE_MESSAGE(r.has_value(), r.error().format());
    CHECK(r->kind() == ImageKind::InMemory);
    CHECK(r->bytes().size() == static_cast<std::size_t>(kImageSize));
    check_directories(*r);

    const auto null_base = Image::parse_mapped_module(nullptr);
    REQUIRE_FALSE(null_base.has_value());
    CHECK(null_base.error().code == 1318u);
}

TEST_CASE("pe synthetic: an absent directory is empty, not an error") {
    PeBytes disk = build_pe_on_disk();
    for (std::size_t index : {0u, 1u, 3u, 6u, 9u, 10u}) {
        w32(disk, kDirectories + index * 8, 0);
        w32(disk, kDirectories + index * 8 + 4, 0);
    }
    const Image img = parse_ok(disk, ImageKind::OnDisk);
    CHECK_FALSE(img.exports()->present);
    CHECK(img.exports()->entries.empty());
    CHECK(img.imports()->empty());
    CHECK(img.debug_entries()->empty());
    CHECK_FALSE(img.codeview()->present);
    CHECK(img.exception_table()->entries.empty());
    CHECK_FALSE(img.load_config()->present);
    CHECK_FALSE(img.has_tls());
    CHECK_FALSE(img.tls()->present);
}

TEST_CASE("pe synthetic: in-memory image without an import lookup table cannot name imports") {
    PeBytes mem = build_pe_in_memory(build_pe_on_disk());
    w32(mem, kImportDir + 0, 0);  // OriginalFirstThunk = 0
    const Image img = parse_ok(mem, ImageKind::InMemory);
    const auto im = img.imports();
    REQUIRE(im.has_value());
    REQUIRE(im->size() == 1u);
    CHECK_FALSE((*im)[0].names_available);
    CHECK((*im)[0].entries.empty());
    CHECK((*im)[0].iat_rva == kIat);

    // On disk the IAT still holds the unbound thunks, so it serves as the lookup table.
    PeBytes disk = build_pe_on_disk();
    w32(disk, rdata_off(kImportDir) + 0, 0);
    const Image d = parse_ok(disk, ImageKind::OnDisk);
    const auto dim = d.imports();
    REQUIRE(dim.has_value());
    REQUIRE(dim->size() == 1u);
    CHECK((*dim)[0].names_available);
    REQUIRE((*dim)[0].entries.size() == 2u);
    CHECK((*dim)[0].entries[0].name == "GetTickCount64");
}

TEST_CASE("pe synthetic: unsorted exception table falls back to a linear search") {
    PeBytes disk = build_pe_on_disk();
    // Swap entries 0 and 2.
    for (std::size_t k = 0; k < 3; ++k) {
        std::uint32_t a = 0;
        std::uint32_t c = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            a |= static_cast<std::uint32_t>(disk[kPdataRaw + k * 4 + i]) << (8 * i);
            c |= static_cast<std::uint32_t>(disk[kPdataRaw + 24 + k * 4 + i]) << (8 * i);
        }
        w32(disk, kPdataRaw + k * 4, c);
        w32(disk, kPdataRaw + 24 + k * 4, a);
    }
    const Image img = parse_ok(disk, ImageKind::OnDisk);
    const auto exc = img.exception_table();
    REQUIRE(exc.has_value());
    CHECK_FALSE(exc->sorted);
    const RuntimeFunction* f = exc->find(0x1005);
    REQUIRE(f != nullptr);
    CHECK(f->begin_rva == 0x1000u);
}

// ---- malformed input -------------------------------------------------------------------------

TEST_CASE("pe malformed: headers") {
    const PeBytes good = build_pe_on_disk();
    for (ImageKind kind : {ImageKind::OnDisk, ImageKind::InMemory}) {
        expect_parse_error(PeBytes(), kind, 1301);
        expect_parse_error(PeBytes(63, 0), kind, 1301);

        PeBytes b = good;
        w16(b, 0, 0x4D4D);
        expect_parse_error(b, kind, 1301);

        b = good;
        w32(b, 0x3C, 0xFFFFFFF0u);
        expect_parse_error(b, kind, 1302);
        w32(b, 0x3C, static_cast<std::uint32_t>(good.size()) - 10);
        expect_parse_error(b, kind, 1302);

        b = good;
        w32(b, kLfanew, 0x00004551);
        expect_parse_error(b, kind, 1302);

        b = good;
        w16(b, kOptional, 0x10B);
        expect_parse_error(b, kind, 1303);

        b = good;
        w16(b, kOptional, 0x107);
        expect_parse_error(b, kind, 1304);

        b = good;
        w16(b, kFileHeader + 16, 50);  // optional header smaller than the fixed part
        expect_parse_error(b, kind, 1305);

        b = good;
        w16(b, kFileHeader + 16, 0xFFF0);  // optional header runs past the file
        expect_parse_error(b, kind, 1305);

        b = good;
        w32(b, kOptional + 108, 16);
        w16(b, kFileHeader + 16, 112 + 8 * 4);  // 16 directories declared, only 4 fit
        expect_parse_error(b, kind, 1305);

        b = good;
        w16(b, kFileHeader + 2, 0xFFFF);  // far more sections than the file holds
        expect_parse_error(b, kind, 1306);
    }
    PeBytes cut = good;
    cut.resize(kSections + 70);  // section table cut mid-entry
    expect_parse_error(cut, ImageKind::OnDisk, 1306);
}

TEST_CASE("pe malformed: unmapped and out-of-range reads") {
    const PeBytes disk = build_pe_on_disk();
    const Image d = parse_ok(disk, ImageKind::OnDisk);

    auto r = d.read_rva(0x0F00, 4);  // gap between headers and .text
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1307u);
    r = d.read_rva(kImageSize + 0x100, 4);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1307u);
    r = d.read_rva(kPdataRva + 0x40, 4);  // first RVA past .pdata's raw data
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1307u);

    r = d.read_rva(kTextRva + 0x1F0, 0x20);  // crosses the end of .text
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1308u);
    r = d.read_rva(kTextRva, 0xFFFFFFFFFFFFull);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1308u);
    CHECK(d.read_rva(kTextRva, 0x200).has_value());
    CHECK(d.read_rva(kTextRva, 0).has_value());

    const auto s = d.read_cstring_rva(kTextRva);  // 0xCC filler never terminates inside .text
    REQUIRE_FALSE(s.has_value());
    CHECK(s.error().code == 1309u);
    CHECK(d.read_cstring_rva(kDllName).has_value());

    // Truncated file: sections reach past EOF.
    PeBytes t = disk;
    t.resize(0x600);
    const Image trunc = parse_ok(t, ImageKind::OnDisk);
    const auto cut = trunc.read_rva(kRdataRva + 0x400, 4);  // file offset 0x800 > 0x600
    REQUIRE_FALSE(cut.has_value());
    CHECK(cut.error().code == 1307u);
    CHECK_FALSE(trunc.exports().has_value());

    const PeBytes mem = build_pe_in_memory(disk);
    const Image m = parse_ok(mem, ImageKind::InMemory);
    const auto mr = m.read_rva(kImageSize - 2, 4);
    REQUIRE_FALSE(mr.has_value());
    CHECK(mr.error().code == 1308u);
    const auto mu = m.read_rva(kImageSize, 1);
    REQUIRE_FALSE(mu.has_value());
    CHECK(mu.error().code == 1307u);
}

TEST_CASE("pe malformed: directory contents") {
    for (ImageKind kind : {ImageKind::OnDisk, ImageKind::InMemory}) {
        const PeBytes disk = build_pe_on_disk();
        const PeBytes good = kind == ImageKind::OnDisk ? disk : build_pe_in_memory(disk);
        const auto at = [&](std::uint32_t rva) {
            return kind == ImageKind::OnDisk ? rdata_off(rva) : static_cast<std::size_t>(rva);
        };

        // Exports.
        PeBytes b = good;
        w16(b, at(kExportOrdinals) + 2, 9);  // ordinal index beyond NumberOfFunctions
        auto ex = parse_ok(b, kind).exports();
        REQUIRE_FALSE(ex.has_value());
        CHECK(ex.error().code == 1310u);

        b = good;
        w32(b, at(kExportDir) + 28, 0x9000);  // address table RVA unmapped
        ex = parse_ok(b, kind).exports();
        REQUIRE_FALSE(ex.has_value());
        CHECK(ex.error().code == 1310u);

        b = good;
        w32(b, at(kExportDir) + 20, 0x7FFFFFFF);  // absurd function count
        ex = parse_ok(b, kind).exports();
        REQUIRE_FALSE(ex.has_value());
        CHECK(ex.error().code == 1310u);

        if (kind == ImageKind::OnDisk) {
            // Name pointer into .text: no terminator before the section ends. (A mapped image is
            // one contiguous region, so the same string would end at the next zero byte there.)
            b = good;
            w32(b, at(kExportNames), kTextRva);
            ex = parse_ok(b, kind).exports();
            REQUIRE_FALSE(ex.has_value());
            CHECK(ex.error().code == 1310u);
        }

        // Imports.
        b = good;
        w64(b, at(kIlt), 0x7FFF0000);  // hint/name RVA unmapped
        auto im = parse_ok(b, kind).imports();
        REQUIRE_FALSE(im.has_value());
        CHECK(im.error().code == 1311u);

        b = good;
        w32(b, at(kImportDir) + 12, 0);  // descriptor without a DLL name
        im = parse_ok(b, kind).imports();
        REQUIRE_FALSE(im.has_value());
        CHECK(im.error().code == 1311u);

        b = good;
        w32(b, at(kImportDir) + 20 + 16, 0x1234);  // terminator descriptor is not null: next one has no name
        im = parse_ok(b, kind).imports();
        REQUIRE_FALSE(im.has_value());
        CHECK(im.error().code == 1311u);

        // Debug.
        b = good;
        w32(b, at(kDebugDir) + 20, 0x9000);  // data RVA unmapped
        auto cv = parse_ok(b, kind).codeview();
        REQUIRE_FALSE(cv.has_value());
        CHECK(cv.error().code == 1312u);

        b = good;
        w32(b, at(kRsds), 0x3031424E);  // 'NB10'
        cv = parse_ok(b, kind).codeview();
        REQUIRE_FALSE(cv.has_value());
        CHECK(cv.error().code == 1313u);

        b = good;
        w32(b, at(kDebugDir) + 16, 20);  // record too short for RSDS
        cv = parse_ok(b, kind).codeview();
        REQUIRE_FALSE(cv.has_value());
        CHECK(cv.error().code == 1312u);

        b = good;
        w32(b, at(kDebugDir) + 16, 30);  // path cut before its NUL
        cv = parse_ok(b, kind).codeview();
        REQUIRE_FALSE(cv.has_value());
        CHECK(cv.error().code == 1312u);

        // Exception.
        b = good;
        w32(b, kDirectories + 3 * 8, 0x9000);
        auto exc = parse_ok(b, kind).exception_table();
        REQUIRE_FALSE(exc.has_value());
        CHECK(exc.error().code == 1314u);

        // Load config.
        b = good;
        w32(b, at(kLoadConfig), 2);  // Size below the minimum
        auto lc = parse_ok(b, kind).load_config();
        REQUIRE_FALSE(lc.has_value());
        CHECK(lc.error().code == 1315u);

        b = good;
        w32(b, kDirectories + 10 * 8, 0x9000);
        lc = parse_ok(b, kind).load_config();
        REQUIRE_FALSE(lc.has_value());
        CHECK(lc.error().code == 1315u);

        // A load config that predates GuardFlags is valid and reports no flags.
        b = good;
        w32(b, at(kLoadConfig), 0x40);
        w32(b, kDirectories + 10 * 8 + 4, 0x40);
        lc = parse_ok(b, kind).load_config();
        REQUIRE(lc.has_value());
        CHECK_FALSE(lc->has_guard_flags);
        CHECK(lc->guard_flags == 0u);
        CHECK_FALSE(lc->cf_instrumented());

        // TLS.
        b = good;
        w32(b, kDirectories + 9 * 8 + 4, 10);
        auto tls = parse_ok(b, kind).tls();
        REQUIRE_FALSE(tls.has_value());
        CHECK(tls.error().code == 1316u);
        b = good;
        w32(b, kDirectories + 9 * 8, 0x9000);
        tls = parse_ok(b, kind).tls();
        REQUIRE_FALSE(tls.has_value());
        CHECK(tls.error().code == 1316u);
    }
}

TEST_CASE("pe malformed: missing file") {
    const auto r = load_image_file("/definitely/not/here.exe");
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().code == 1300u);
    CHECK(r.error().message.find("here.exe") != std::string::npos);
}

TEST_CASE("pe robustness: every truncation and every byte flip is handled") {
    const PeBytes disk = build_pe_on_disk();
    const PeBytes mem = build_pe_in_memory(disk);
    for (const auto& pair : {std::pair<const PeBytes*, ImageKind>{&disk, ImageKind::OnDisk},
                             std::pair<const PeBytes*, ImageKind>{&mem, ImageKind::InMemory}}) {
        const PeBytes& base = *pair.first;
        const ImageKind kind = pair.second;
        // Truncations (every length up to the headers, then every 7th).
        for (std::size_t len = 0; len < base.size(); len += (len < 0x220 ? 1 : 7)) {
            const PeBytes t(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(len));
            const auto r = Image::parse(pe_span(t), kind);
            if (r.has_value()) {
                exercise(*r);
            } else {
                CHECK(r.error().valid());
            }
        }
        // Flips: all header bytes, then the structure-bearing parts of .rdata / .pdata.
        for (std::size_t pos = 0; pos < base.size(); pos += (pos < 0x200 ? 1 : 11)) {
            for (std::uint8_t mask : {std::uint8_t{0xFF}, std::uint8_t{0x80}, std::uint8_t{0x01}}) {
                PeBytes t = base;
                t[pos] = static_cast<std::uint8_t>(t[pos] ^ mask);
                const auto r = Image::parse(pe_span(t), kind);
                if (r.has_value()) {
                    exercise(*r);
                } else {
                    CHECK(r.error().valid());
                }
            }
        }
    }
}

TEST_CASE("pe synthetic: only min(VirtualSize, SizeOfRawData) bytes are file-backed") {
    const std::size_t pdata_header = kSections + 2 * 40;
    // (a) VirtualSize 0x80 > SizeOfRawData 0x40: the tail is zero-fill.
    PeBytes disk = build_pe_on_disk();
    w32(disk, pdata_header + 8, 0x80);
    const Image d = parse_ok(disk, ImageKind::OnDisk);
    CHECK(d.section_for_rva(kPdataRva + 0x7F) == &d.sections()[2]);
    CHECK(d.read_rva(kPdataRva + 0x3C, 4).has_value());
    CHECK(d.rva_to_offset(kPdataRva + 0x3F).has_value());
    CHECK_FALSE(d.rva_to_offset(kPdataRva + 0x40).has_value());
    const auto zf = d.read_rva(kPdataRva + 0x40, 4);
    REQUIRE_FALSE(zf.has_value());
    CHECK(zf.error().code == 1307u);
    CHECK(d.read_rva(kPdataRva + 0x3E, 4).error().code == 1308u);  // crosses from file-backed into zero-fill
    // In memory the loader's zero-fill is real memory, so it reads as zeros.
    PeBytes mem = build_pe_in_memory(disk);
    const Image m = parse_ok(mem, ImageKind::InMemory);
    const auto mz = m.read_rva(kPdataRva + 0x40, 4);
    REQUIRE(mz.has_value());
    CHECK(std::to_integer<int>((*mz)[0]) == 0);

    // (b) VirtualSize 0x20 < SizeOfRawData 0x40: raw bytes past VirtualSize are padding, never mapped.
    PeBytes disk2 = build_pe_on_disk();
    w32(disk2, pdata_header + 8, 0x20);
    const Image d2 = parse_ok(disk2, ImageKind::OnDisk);
    CHECK(d2.section_for_rva(kPdataRva + 0x1F) == &d2.sections()[2]);
    CHECK(d2.section_for_rva(kPdataRva + 0x20) == nullptr);
    CHECK(d2.read_rva(kPdataRva + 0x1C, 4).has_value());
    CHECK(d2.read_rva(kPdataRva + 0x1E, 4).error().code == 1308u);
    CHECK(d2.read_rva(kPdataRva + 0x20, 4).error().code == 1307u);
    CHECK_FALSE(d2.offset_to_rva(kPdataRaw + 0x20).has_value());
    CHECK(d2.offset_to_rva(kPdataRaw + 0x1F) == kPdataRva + 0x1Fu);
}

TEST_CASE("pe synthetic: load config uses the structure's own Size, not the directory size") {
    for (ImageKind kind : {ImageKind::OnDisk, ImageKind::InMemory}) {
        const PeBytes disk = build_pe_on_disk();
        const PeBytes good = kind == ImageKind::OnDisk ? disk : build_pe_in_memory(disk);
        PeBytes b = good;
        w32(b, kDirectories + 10 * 8 + 4, 0x10);  // directory claims 16 bytes, structure Size says 0x98
        auto lc = parse_ok(b, kind).load_config();
        REQUIRE_MESSAGE(lc.has_value(), lc.error().format());
        CHECK(lc->has_guard_flags);
        CHECK(lc->guard_flags == (guard::CfInstrumented | guard::CfFunctionTablePresent));
        CHECK(lc->guard_cf_function_count == 5u);

        // A Size larger than the image has bytes for is bounded by what is available.
        b = good;
        w32(b, kind == ImageKind::OnDisk ? rdata_off(kLoadConfig) : static_cast<std::size_t>(kLoadConfig), 0x7FFFFFFF);
        lc = parse_ok(b, kind).load_config();
        REQUIRE_MESSAGE(lc.has_value(), lc.error().format());
        CHECK(lc->size == 0x7FFFFFFFu);
        CHECK(lc->guard_flags == (guard::CfInstrumented | guard::CfFunctionTablePresent));
    }
}

TEST_CASE("pe synthetic: bound imports without an import name table fail cleanly") {
    for (ImageKind kind : {ImageKind::OnDisk, ImageKind::InMemory}) {
        const PeBytes disk = build_pe_on_disk();
        PeBytes b = kind == ImageKind::OnDisk ? disk : build_pe_in_memory(disk);
        const std::size_t imp = kind == ImageKind::OnDisk ? rdata_off(kImportDir) : static_cast<std::size_t>(kImportDir);
        w32(b, imp + 0, 0);           // OriginalFirstThunk = 0
        w32(b, imp + 4, 0x5F123456);  // TimeDateStamp != 0: bound
        const auto im = parse_ok(b, kind).imports();
        REQUIRE_FALSE(im.has_value());
        CHECK(im.error().code == 1311u);
        CHECK(im.error().message.find("bound imports") != std::string::npos);
    }
}

TEST_CASE("pe synthetic: CodeView scan skips non-RSDS entries and finds a later RSDS") {
    for (ImageKind kind : {ImageKind::OnDisk, ImageKind::InMemory}) {
        const PeBytes disk = build_pe_on_disk();
        PeBytes b = kind == ImageKind::OnDisk ? disk : build_pe_in_memory(disk);
        const auto at = [&](std::uint32_t rva) {
            return kind == ImageKind::OnDisk ? rdata_off(rva) : static_cast<std::size_t>(rva);
        };
        // Entry 0: the existing record, turned into NB10. Entry 1: a fresh RSDS record at 0x2780.
        w32(b, at(kRsds), 0x3031424E);
        const std::size_t e0 = at(kDebugDir);
        const std::size_t e1 = e0 + 28;
        for (std::size_t i = 0; i < 28; ++i) {
            b[e1 + i] = b[e0 + i];
        }
        w32(b, e1 + 20, 0x2780);
        w32(b, e1 + 24, static_cast<std::uint32_t>(rdata_off(0x2780)));
        const std::size_t rec = at(0x2780);
        w32(b, rec, 0x53445352);
        for (std::size_t i = 0; i < 16; ++i) {
            b[rec + 4 + i] = static_cast<std::uint8_t>(0xA0 + i);
        }
        w32(b, rec + 20, 7);
        wstr(b, rec + 24, "C:\\build\\test.pdb");
        w32(b, kDirectories + 6 * 8 + 4, 56);  // two entries

        const Image img = parse_ok(b, kind);
        const auto cv = img.codeview();
        REQUIRE_MESSAGE(cv.has_value(), cv.error().format());
        REQUIRE(cv->present);
        CHECK(cv->age == 7u);
        CHECK(cv->guid.data4[0] == 0xA8u);
        CHECK(cv->pdb_path == "C:\\build\\test.pdb");

        // With only the NB10 entry left there is no RSDS: CML1313.
        w32(b, kDirectories + 6 * 8 + 4, 28);
        const auto none = parse_ok(b, kind).codeview();
        REQUIRE_FALSE(none.has_value());
        CHECK(none.error().code == 1313u);
    }
}

TEST_CASE("pe: OwnedImage is move-only and a move keeps the views valid") {
    static_assert(!std::is_copy_constructible_v<OwnedImage>);
    static_assert(!std::is_copy_assignable_v<OwnedImage>);
    static_assert(std::is_nothrow_move_constructible_v<OwnedImage>);
    static_assert(std::is_nothrow_move_assignable_v<OwnedImage>);

    const PeBytes disk = build_pe_on_disk();
    std::vector<std::byte> bytes(pe_span(disk).begin(), pe_span(disk).end());
    auto parsed = Image::parse(ByteSpan(bytes.data(), bytes.size()), ImageKind::OnDisk);
    REQUIRE(parsed.has_value());
    const std::byte* heap = bytes.data();
    OwnedImage a(std::move(bytes), std::move(parsed).value());
    OwnedImage b(std::move(a));  // move construct
    CHECK(b.image.bytes().data() == b.bytes.data());
    CHECK(b.bytes.data() == heap);
    const auto ex = b.image.exports();
    REQUIRE(ex.has_value());
    CHECK(ex->find_by_name("alpha") != nullptr);
}
