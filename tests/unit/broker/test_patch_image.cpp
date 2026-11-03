// From fix-up triples to the wire format, and the SECREL pre-application (WP-12, review item 1).
#include "cml/core/patch_image.h"

#include <fstream>
#include <iterator>

#include "../pdb/test_util.h"
#include "../pe/pe_builder.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
using bind::FixupKind;
using bind::FixupTriple;

namespace {

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

std::vector<std::uint8_t> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// What the agent computes for a Rel32 fix-up (agent/src/loader/image_writer.cpp, ADR-0025).
std::int32_t agent_displacement(const ipc::Fixup& f, std::uint64_t base) {
    const std::uint64_t site = base + f.rva;
    return static_cast<std::int32_t>(f.value - (site + 4 + f.n));
}

}  // namespace

TEST_CASE("Rel32 triples become absolute targets the agent turns back into the same displacement") {
    QuietLogs quiet;
    const std::uint64_t base = 0x7FF5'0000'0000ull;
    // value = the displacement make_fixup_triples() computed (two's complement, 32 bits).
    const std::vector<FixupTriple> triples = {
        {0x1010, FixupKind::Rel32, 0x00001000u},
        {0x1020, FixupKind::Rel32_1, static_cast<std::uint32_t>(-0x2000)},
        {0x1030, FixupKind::Rel32_4, 0x7FFFFFFFu},
        {0x1040, FixupKind::Rel32_5, 0x80000000u},
    };
    Result<PatchFixups> r = to_patch_fixups(triples, base);
    REQUIRE(r.has_value());
    REQUIRE(r->wire.size() == 4);
    CHECK(r->pre_applied.empty());
    const std::uint8_t ns[] = {0, 1, 4, 5};
    const std::int32_t disps[] = {0x1000, -0x2000, 0x7FFFFFFF, INT32_MIN};
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(r->wire[i].kind == ipc::FixupKind::Rel32);
        CHECK(r->wire[i].n == ns[i]);
        CHECK(r->wire[i].rva == triples[i].rva);
        CHECK(agent_displacement(r->wire[i], base) == disps[i]);
    }
}

TEST_CASE("Addr64 and Slot map to Addr64 and SlotWrite unchanged") {
    QuietLogs quiet;
    const std::vector<FixupTriple> triples = {{0x2000, FixupKind::Addr64, 0x140001234ull}, {0x3000, FixupKind::Slot, 0x140005678ull}};
    Result<PatchFixups> r = to_patch_fixups(triples, 0x180000000ull);
    REQUIRE(r.has_value());
    REQUIRE(r->wire.size() == 2);
    CHECK(r->wire[0].kind == ipc::FixupKind::Addr64);
    CHECK(r->wire[0].value == 0x140001234ull);
    CHECK(r->wire[0].n == 0);
    CHECK(r->wire[1].kind == ipc::FixupKind::SlotWrite);
    CHECK(r->wire[1].value == 0x140005678ull);
}

TEST_CASE("SecRel32 is not sent to the agent: it is pre-applied to the DLL") {
    QuietLogs quiet;
    const std::vector<FixupTriple> triples = {{0x1100, FixupKind::SecRel32, 0x24}, {0x1200, FixupKind::Addr64, 5}};
    Result<PatchFixups> r = to_patch_fixups(triples, 0x180000000ull);
    REQUIRE(r.has_value());
    CHECK(r->wire.size() == 1);
    REQUIRE(r->pre_applied.size() == 1);
    CHECK(r->pre_applied[0].rva == 0x1100);
    CHECK(r->pre_applied[0].value == 0x24);
    // A value that does not fit 32 bits is refused with the planner's range diagnostic.
    Result<PatchFixups> bad = to_patch_fixups(std::vector<FixupTriple>{{0x1100, FixupKind::SecRel32, 0x1'0000'0000ull}}, 0x180000000ull);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().id() == "CML3205");
}

TEST_CASE("preapply_write32 writes the value at the file offset of the RVA and nothing else") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string path = dir.file("patch.dll");
    const cml_test::PeBytes original = cml_test::build_pe_on_disk();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(original.data()), static_cast<std::streamsize>(original.size()));
    }
    // .text: RVA 0x1000 is file offset 0x200.
    const std::vector<Write32> writes = {{0x1100, 0xAABBCCDDu}, {0x1104, 0x01020304u}};
    Result<void> r = preapply_write32(path, writes);
    REQUIRE(r.has_value());
    const std::vector<std::uint8_t> now = read_all(path);
    REQUIRE(now.size() == original.size());
    std::size_t differing = 0;
    for (std::size_t i = 0; i < now.size(); ++i) {
        differing += now[i] != original[i] ? 1 : 0;
    }
    CHECK(now[0x200 + 0x100] == 0xDD);
    CHECK(now[0x200 + 0x101] == 0xCC);
    CHECK(now[0x200 + 0x102] == 0xBB);
    CHECK(now[0x200 + 0x103] == 0xAA);
    CHECK(now[0x200 + 0x104] == 0x04);
    CHECK(now[0x200 + 0x107] == 0x01);
    CHECK(differing <= 8);  // 0xCC filler bytes may already equal some of the new ones
}

TEST_CASE("preapply_write32 refuses a bad site before it writes anything") {
    QuietLogs quiet;
    testpdb::TempDir dir;
    const std::string path = dir.file("patch.dll");
    const cml_test::PeBytes original = cml_test::build_pe_on_disk();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(original.data()), static_cast<std::streamsize>(original.size()));
    }
    SUBCASE("outside every section") {
        const std::vector<Write32> writes = {{0x1100, 0x11111111u}, {0x9000, 1}};
        Result<void> r = preapply_write32(path, writes);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML5310");
        CHECK(read_all(path) == std::vector<std::uint8_t>(original.begin(), original.end()));
    }
    SUBCASE("straddling the end of the raw data of a section") {
        const std::vector<Write32> writes = {{0x11FE, 1}};
        Result<void> r = preapply_write32(path, writes);
        REQUIRE_FALSE(r.has_value());
        CHECK(read_all(path) == std::vector<std::uint8_t>(original.begin(), original.end()));
    }
    SUBCASE("a file that is not a PE image") {
        const std::string junk = dir.file("junk.dll");
        std::ofstream(junk, std::ios::binary) << "not a PE";
        Result<void> r = preapply_write32(junk, std::vector<Write32>{{0x1000, 1}});
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().id() == "CML5310");
    }
    SUBCASE("nothing to write is not an error, even for a file that does not exist") {
        CHECK(preapply_write32(dir.file("absent.dll"), {}).has_value());
    }
}
