// Opening a PDB from a PE image: RSDS extraction, PDB search order, GUID/age validation.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "cml/pdb/pdb.h"
#include <ostream>  // doctest streams string_view operands; MSVC needs the full definition

#include "doctest.h"
#include "test_util.h"

using namespace cml::testpdb;
using cml::pdb::OpenOptions;
using cml::pdb::PdbIndex;
namespace dg = cml::pdb::diag;

namespace {

const std::array<std::uint8_t, 16> kGuid = {0x3C, 0x83, 0xC3, 0x8C, 0x00, 0x87, 0x0D, 0xEB,
                                            0x4C, 0x4C, 0x44, 0x20, 0x50, 0x44, 0x42, 0x2E};

OpenOptions no_cache() {
    OpenOptions o;
    o.use_cache = false;
    return o;
}

// Paths are built with mixed separators in these tests; compare them in normalised form.
bool same_path(const std::string& a, const std::string& b) {
    return cml::pdb::normalize_path(a) == cml::pdb::normalize_path(b);
}

}  // namespace

TEST_CASE("RSDS extraction from a PE image") {
    const Bytes pe = build_pe(kGuid, 5, "C:\\build\\app.pdb");
    auto ref = cml::pdb::read_codeview_ref_from_image(pe.data(), pe.size());
    REQUIRE(ref);
    CHECK(ref->id.guid == kGuid);
    CHECK(ref->id.age == 5);
    CHECK(ref->pdb_path == "C:\\build\\app.pdb");

    SUBCASE("no debug directory") {
        const Bytes none = build_pe(kGuid, 5, "x.pdb", false);
        auto r = cml::pdb::read_codeview_ref_from_image(none.data(), none.size());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kNoCodeViewRecord);
    }
    SUBCASE("not a PE") {
        const Bytes junk(1000, 0x41);
        auto r = cml::pdb::read_codeview_ref_from_image(junk.data(), junk.size());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kBadPeImage);
        auto n = cml::pdb::read_codeview_ref_from_image(nullptr, 0);
        REQUIRE_FALSE(n);
        CHECK(n.error().code == dg::kBadPeImage);
    }
    SUBCASE("a record that is not RSDS is skipped") {
        Bytes other = pe;
        other[0x240] = 'N';  // NB10-like
        auto r = cml::pdb::read_codeview_ref_from_image(other.data(), other.size());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kNoCodeViewRecord);
    }
    SUBCASE("unterminated path is bounded by the record") {
        Bytes unterminated = pe;
        // Overwrite the path terminator and what follows with non-zero bytes up to the record end.
        const std::size_t record_end = 0x240 + 24 + std::string("C:\\build\\app.pdb").size() + 1;
        for (std::size_t i = 0x240 + 24; i < record_end; ++i) {
            unterminated[i] = 'A';
        }
        auto r = cml::pdb::read_codeview_ref_from_image(unterminated.data(), unterminated.size());
        REQUIRE(r);
        CHECK(r->pdb_path.size() == record_end - (0x240 + 24));
    }
}

TEST_CASE("RSDS extraction survives truncation and corruption") {
    const Bytes pe = build_pe(kGuid, 5, "C:\\build\\app.pdb");
    for (std::size_t n = 0; n < pe.size(); ++n) {
        auto r = cml::pdb::read_codeview_ref_from_image(pe.data(), n);
        // Only the full image (or one that keeps the whole CodeView record) can succeed.
        if (n < 0x240 + 24 + 17) {
            CHECK_FALSE(r);
        }
        if (!r) {
            CHECK(r.error().code >= 1400);
            CHECK(r.error().code <= 1499);
        }
    }
    std::uint64_t s = 0x2545F4914F6CDD1Dull;
    for (int iter = 0; iter < 3000; ++iter) {
        Bytes b = pe;
        for (int f = 0; f < 4; ++f) {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            b[static_cast<std::size_t>(s % b.size())] = static_cast<std::uint8_t>(s >> 32);
        }
        (void)cml::pdb::read_codeview_ref_from_image(b.data(), b.size());
    }
}

TEST_CASE("open_for_pe: search order and validation") {
    TempDir dir;
    const std::string bin = dir.subdir("bin");
    const std::string elsewhere = dir.subdir("elsewhere");
    const std::string symbols = dir.subdir("symbols");
    const Bytes good_pdb = build(standard_spec());
    Spec other_guid = standard_spec();
    other_guid.guid[0] ^= 0x01;
    Spec other_age = standard_spec();
    other_age.age = 9;

    SUBCASE("recorded absolute path") {
        const std::string recorded = elsewhere + "/app.pdb";
        write_file(recorded, good_pdb);
        write_file(bin + "/app.exe", build_pe(kGuid, 1, recorded));
        auto r = PdbIndex::open_for_pe(bin + "/app.exe", no_cache());
        REQUIRE(r);
        CHECK(same_path((*r)->path(), recorded));
        CHECK((*r)->compiland_count() == 5);
    }
    SUBCASE("next to the image when the recorded path does not exist") {
        write_file(bin + "/app.pdb", good_pdb);
        write_file(bin + "/app.exe", build_pe(kGuid, 1, elsewhere + "/gone/app.pdb"));
        auto r = PdbIndex::open_for_pe(bin + "/app.exe", no_cache());
        REQUIRE(r);
        CHECK(same_path((*r)->path(), bin + "/app.pdb"));
    }
    SUBCASE("a relative recorded path is relative to the image") {
        write_file(bin + "/app.pdb", good_pdb);
        write_file(bin + "/app.exe", build_pe(kGuid, 1, "app.pdb"));
        auto r = PdbIndex::open_for_pe(bin + "/app.exe", no_cache());
        REQUIRE(r);
        CHECK(same_path((*r)->path(), bin + "/app.pdb"));
    }
    SUBCASE("override directories") {
        write_file(symbols + "/app.pdb", good_pdb);
        write_file(bin + "/app.exe", build_pe(kGuid, 1, elsewhere + "/gone/app.pdb"));
        OpenOptions opts = no_cache();
        opts.override_dirs = {elsewhere, symbols};
        auto r = PdbIndex::open_for_pe(bin + "/app.exe", opts);
        REQUIRE(r);
        CHECK(same_path((*r)->path(), symbols + "/app.pdb"));
        // Without the override directory it is not found.
        auto miss = PdbIndex::open_for_pe(bin + "/app.exe", no_cache());
        REQUIRE_FALSE(miss);
        CHECK(miss.error().code == dg::kPdbNotFound);
        CHECK(miss.error().message.find("app.pdb") != std::string::npos);
        CHECK_FALSE(miss.error().what_to_do.empty());
    }
    SUBCASE("a mismatching candidate is skipped when a later one matches") {
        const std::string recorded = elsewhere + "/app.pdb";
        write_file(recorded, build(other_guid));
        write_file(symbols + "/app.pdb", good_pdb);
        write_file(bin + "/app.exe", build_pe(kGuid, 1, recorded));
        OpenOptions opts = no_cache();
        opts.override_dirs = {symbols};
        auto r = PdbIndex::open_for_pe(bin + "/app.exe", opts);
        REQUIRE(r);
        CHECK(same_path((*r)->path(), symbols + "/app.pdb"));
    }
    SUBCASE("GUID mismatch") {
        const std::string recorded = elsewhere + "/app.pdb";
        write_file(recorded, build(other_guid));
        write_file(bin + "/app.exe", build_pe(kGuid, 1, recorded));
        auto r = PdbIndex::open_for_pe(bin + "/app.exe", no_cache());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kPdbMismatch);
        CHECK(r.error().id() == "CML1411");
        CHECK(r.error().message.find(recorded) != std::string::npos);
        CHECK(r.error().message.find("8CC3833C-8700-EB0D-4C4C-44205044422E/1") != std::string::npos);  // the wanted id
    }
    SUBCASE("age mismatch") {
        const std::string recorded = elsewhere + "/app.pdb";
        write_file(recorded, build(other_age));
        write_file(bin + "/app.exe", build_pe(kGuid, 1, recorded));
        auto r = PdbIndex::open_for_pe(bin + "/app.exe", no_cache());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kPdbMismatch);
    }
    SUBCASE("image without a debug directory") {
        write_file(bin + "/app.exe", build_pe(kGuid, 1, "x.pdb", false));
        auto r = PdbIndex::open_for_pe(bin + "/app.exe", no_cache());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kNoCodeViewRecord);
    }
    SUBCASE("image that is not a PE") {
        write_file(bin + "/app.exe", good_pdb);
        auto r = PdbIndex::open_for_pe(bin + "/app.exe", no_cache());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kBadPeImage);
    }
    SUBCASE("image that does not exist") {
        auto r = PdbIndex::open_for_pe(bin + "/nothing.exe", no_cache());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kCannotOpenFile);
    }
    SUBCASE("a corrupt PDB candidate is skipped, not fatal") {
        const std::string recorded = elsewhere + "/app.pdb";
        write_file(recorded, Bytes(5000, 7));
        write_file(bin + "/app.pdb", good_pdb);
        write_file(bin + "/app.exe", build_pe(kGuid, 1, recorded));
        auto r = PdbIndex::open_for_pe(bin + "/app.exe", no_cache());
        REQUIRE(r);
        CHECK(same_path((*r)->path(), bin + "/app.pdb"));
    }
    SUBCASE("the cache is keyed by the validated identity") {
        write_file(bin + "/app.pdb", good_pdb);
        write_file(bin + "/app.exe", build_pe(kGuid, 1, bin + "/app.pdb"));
        PdbIndex::cache_clear();
        auto by_pe = PdbIndex::open_for_pe(bin + "/app.exe");
        REQUIRE(by_pe);
        auto by_path = PdbIndex::open_pdb(bin + "/app.pdb");
        REQUIRE(by_path);
        CHECK(by_pe->get() == by_path->get());
    }
}
