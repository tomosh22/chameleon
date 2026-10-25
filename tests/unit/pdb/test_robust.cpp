// Malformed input must produce a CML14xx diagnostic and never crash.
#include <algorithm>
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

OpenOptions no_cache() {
    OpenOptions o;
    o.use_cache = false;
    return o;
}

cml::Result<std::shared_ptr<const PdbIndex>> open_bytes(const TempDir& dir, const Bytes& bytes) {
    const std::string path = dir.file("x.pdb");
    write_file(path, bytes);
    return PdbIndex::open_pdb(path, no_cache());
}

// Touches every query once; none may crash, errors are fine.
void exercise(const PdbIndex& idx) {
    for (std::uint32_t i = 0; i < idx.compiland_count(); ++i) {
        const auto& c = idx.compiland(i);
        (void)c.cwd();
        (void)c.optimization();
        (void)idx.compiland_files(i);
        (void)idx.compiland_files_normalized(i);
        (void)idx.compiland_symbols(i);
        (void)idx.build_env(i);
    }
    (void)idx.compilands_for_file("c:\\proj\\src\\logic.h");
    (void)idx.find_public("main");
    (void)idx.find_global("g_tick");
    (void)idx.publics_at_rva(0x1100);
    (void)idx.public_containing_rva(0x1100);
    (void)idx.find_proc_by_rva(0x1105);
    (void)idx.compiland_for_rva(0x1100);
    (void)idx.load_all(2);
}

struct Xorshift {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

}  // namespace

TEST_CASE("malformed files: diagnostics, not crashes") {
    TempDir dir;
    SUBCASE("missing file") {
        auto r = PdbIndex::open_pdb(dir.file("does_not_exist.pdb"), no_cache());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kCannotOpenFile);
        CHECK(r.error().id() == "CML1400");
        CHECK_FALSE(r.error().what_to_do.empty());
        CHECK(r.error().message.find("does_not_exist.pdb") != std::string::npos);
    }
    SUBCASE("empty file") {
        auto r = open_bytes(dir, Bytes());
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kNotAPdb);
    }
    SUBCASE("tiny file") {
        auto r = open_bytes(dir, Bytes(10, 0x41));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kNotAPdb);
    }
    SUBCASE("wrong magic") {
        Bytes b = build(standard_spec());
        b[0] = 'X';
        auto r = open_bytes(dir, b);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kNotAPdb);
        CHECK_FALSE(cml::pdb::is_valid_pdb_image(b.data(), b.size()));
    }
    SUBCASE("zeroes") {
        auto r = open_bytes(dir, Bytes(8192, 0));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kNotAPdb);
    }
    SUBCASE("old MSF 2.00 PDB") {
        Bytes b(4096, 0);
        const char magic[] = "Microsoft C/C++ program database 2.00\r\n\x1a" "JG\0\0";
        std::memcpy(b.data(), magic, sizeof magic);
        auto r = open_bytes(dir, b);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kUnsupportedPdbFormat);
    }
    SUBCASE("invalid block size") {
        Bytes b = build(standard_spec());
        b[32] = 0x01;  // block size 4097 (not a power of two)
        b[33] = 0x10;
        auto r = open_bytes(dir, b);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kUnsupportedPdbFormat);
    }
    SUBCASE("huge directory size") {
        Bytes b = build(standard_spec());
        b[44] = 0xFF;
        b[45] = 0xFF;
        b[46] = 0xFF;
        b[47] = 0x7F;
        auto r = open_bytes(dir, b);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kCorruptStreamDirectory);
    }
    SUBCASE("block map address outside the file") {
        Bytes b = build(standard_spec());
        b[52] = 0xFF;
        b[53] = 0xFF;
        auto r = open_bytes(dir, b);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kCorruptStreamDirectory);
    }
    SUBCASE("trailing garbage is ignored") {
        Bytes b = build(standard_spec());
        b.insert(b.end(), 777, 0xEE);
        auto r = open_bytes(dir, b);
        REQUIRE(r);
        CHECK((*r)->compiland_count() == 5);
    }
    SUBCASE("bad DBI signature") {
        Spec s = standard_spec();
        s.bad_dbi_signature = true;
        auto r = open_bytes(dir, build(s));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kBadDbiStream);
    }
    SUBCASE("missing info stream") {
        Spec s = standard_spec();
        s.empty_info_stream = true;
        auto r = open_bytes(dir, build(s));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kBadInfoStream);
    }
    SUBCASE("unknown section contribution version") {
        Spec s = standard_spec();
        s.bad_contrib_version = true;
        auto r = open_bytes(dir, build(s));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kBadSectionContributions);
    }
    SUBCASE("source info offset out of range") {
        Spec s = standard_spec();
        s.bad_fileinfo_offset = true;
        auto r = open_bytes(dir, build(s));
        REQUIRE_FALSE(r);
        CHECK(r.error().code == dg::kBadSourceInfo);
    }
    SUBCASE("corrupt module symbols: open works, that compiland fails, the others still load") {
        Spec s = standard_spec();
        s.modules[2].symbols.raw(Bytes{0xF0, 0xFF, 0x10, 0x11, 0, 0, 0, 0});  // record longer than the stream
        auto r = open_bytes(dir, build(s));
        REQUIRE(r);
        auto bad = (*r)->compiland_symbols(2);
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == dg::kBadModuleSymbols);
        CHECK(bad.error().message.find("rel.cpp.obj") != std::string::npos);
        auto again = (*r)->load_compiland(2);  // the failure is remembered
        REQUIRE_FALSE(again);
        CHECK(again.error().code == dg::kBadModuleSymbols);
        auto good = (*r)->compiland_symbols(0);
        REQUIRE(good);
        CHECK(good->size() == 8);
        auto all = (*r)->load_all();
        REQUIRE_FALSE(all);
        CHECK(all.error().code == dg::kBadModuleSymbols);
    }
    SUBCASE("corrupt symbol record stream: publics fail, everything else works") {
        Spec s = standard_spec();
        s.symrec.raw(Bytes{0xF0, 0xFF, 0x0E, 0x11, 0, 0, 0, 0});
        auto r = open_bytes(dir, build(s));
        REQUIRE(r);
        auto bad = (*r)->find_public("main");
        REQUIRE_FALSE(bad);
        CHECK(bad.error().code == dg::kBadSymbolRecords);
        CHECK_FALSE((*r)->find_global("g_tick"));
        CHECK((*r)->public_count() == 0);
        REQUIRE((*r)->compiland_symbols(0));
    }
    SUBCASE("corrupt IPI: only build_env of a BuildInfo compiland fails") {
        Spec s = standard_spec();
        const Bytes good = build(s);
        // Find the IPI stream header (version 20040203, header size 56) and break its record size.
        Bytes b = good;
        const std::uint8_t pat[8] = {0x0B, 0xCA, 0x31, 0x01, 56, 0, 0, 0};  // 20040203 = 0x0131CA0B
        bool patched = false;
        for (std::size_t i = 0; i + 20 < b.size(); ++i) {
            if (std::memcmp(&b[i], pat, 8) == 0) {
                b[i + 16] = 0xFF;
                b[i + 17] = 0xFF;
                b[i + 18] = 0xFF;
                b[i + 19] = 0x7F;
                patched = true;
                break;
            }
        }
        REQUIRE(patched);
        auto r = open_bytes(dir, b);
        REQUIRE(r);
        auto env = (*r)->build_env(1);
        REQUIRE_FALSE(env);
        CHECK(env.error().code == dg::kBadBuildInfo);
        auto ok = (*r)->build_env(0);  // S_ENVBLOCK does not need the IPI stream
        REQUIRE(ok);
    }
}

TEST_CASE("every diagnostic id is in the CML14xx range and has text") {
    const std::uint32_t ids[] = {dg::kCannotOpenFile,     dg::kNotAPdb,          dg::kUnsupportedPdbFormat, dg::kTruncatedPdb,
                                 dg::kCorruptStreamDirectory, dg::kBadDbiStream, dg::kBadModuleInfo,        dg::kBadInfoStream,
                                 dg::kNoCodeViewRecord,   dg::kBadPeImage,       dg::kPdbNotFound,          dg::kPdbMismatch,
                                 dg::kBadModuleSymbols,   dg::kBadSectionContributions, dg::kBadSymbolRecords, dg::kBadSourceInfo,
                                 dg::kCompilandOutOfRange, dg::kCapacityExceeded, dg::kBadBuildInfo};
    std::vector<std::uint32_t> seen;
    for (std::uint32_t id : ids) {
        CHECK(id >= 1400);
        CHECK(id <= 1499);
        CHECK(std::find(seen.begin(), seen.end(), id) == seen.end());  // unique
        seen.push_back(id);
        const cml::Diag d = cml::Diag::make(id, "m", "w");
        CHECK(d.valid());
        CHECK(d.category() == cml::DiagCategory::Parse);
    }
}

TEST_CASE("truncated PDB: every prefix fails cleanly") {
    for (bool fragmented : {false, true}) {
        TempDir dir;
        Spec spec = standard_spec();
        spec.fragmented = fragmented;
        const Bytes full = build(spec);
        std::size_t failures = 0;
        std::size_t lengths = 0;
        // All block boundaries plus a stride of odd offsets.
        std::vector<std::size_t> cuts;
        for (std::size_t n = 0; n < full.size(); n += 997) {
            cuts.push_back(n);
        }
        for (std::size_t n = 4096; n < full.size(); n += 4096) {
            cuts.push_back(n);
            cuts.push_back(n - 1);
        }
        cuts.push_back(full.size() - 1);
        for (std::size_t n : cuts) {
            Bytes b(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
            auto r = open_bytes(dir, b);
            ++lengths;
            if (!r) {
                ++failures;
                CHECK(r.error().code >= 1400);
                CHECK(r.error().code <= 1499);
            }
        }
        CHECK(failures == lengths);  // a truncated PDB is never accepted
        auto ok = open_bytes(dir, full);
        REQUIRE(ok);
        exercise(**ok);
    }
}

TEST_CASE("random corruption never crashes the reader") {
    for (bool fragmented : {false, true}) {
        TempDir dir;
        Spec spec = standard_spec();
        spec.fragmented = fragmented;
        spec.block_size = 512;  // many small blocks: directory and stream lists get hit often
        const Bytes full = build(spec);
        Xorshift rng{fragmented ? 0x9E3779B97F4A7C15ull : 0xD1B54A32D192ED03ull};
        std::size_t opened = 0;
        for (int iter = 0; iter < 400; ++iter) {
            Bytes b = full;
            const int flips = 1 + static_cast<int>(rng.next() % 6);
            for (int f = 0; f < flips; ++f) {
                // Half of the flips go to the first 256 bytes (super block) and the tail (directory).
                std::size_t pos;
                switch (rng.next() % 3) {
                    case 0: pos = static_cast<std::size_t>(rng.next() % 256); break;
                    case 1: pos = b.size() - 1 - static_cast<std::size_t>(rng.next() % 1024); break;
                    default: pos = static_cast<std::size_t>(rng.next() % b.size()); break;
                }
                b[pos] = static_cast<std::uint8_t>(rng.next());
            }
            auto r = open_bytes(dir, b);
            if (r) {
                ++opened;
                exercise(**r);
            } else {
                CHECK(r.error().code >= 1400);
                CHECK(r.error().code <= 1499);
            }
        }
        CHECK(opened > 0);  // the corruption is not so heavy that nothing ever opens
    }
}

TEST_CASE("random corruption of the stream contents (valid container)") {
    // Corrupt bytes only inside the data area of a contiguous layout, so the container stays valid
    // and the DBI / module / symbol parsers see the damage.
    TempDir dir;
    Spec spec = standard_spec();
    spec.block_size = 512;
    const Bytes full = build(spec);
    Xorshift rng{0x123456789ABCDEFull};
    for (int iter = 0; iter < 600; ++iter) {
        Bytes b = full;
        const int flips = 1 + static_cast<int>(rng.next() % 4);
        for (int f = 0; f < flips; ++f) {
            const std::size_t pos = 3 * 512 + static_cast<std::size_t>(rng.next() % (b.size() - 3 * 512 - 1024));
            b[pos] = static_cast<std::uint8_t>(rng.next());
        }
        auto r = open_bytes(dir, b);
        if (r) {
            exercise(**r);
        } else {
            CHECK(r.error().code >= 1400);
            CHECK(r.error().code <= 1499);
        }
    }
}
