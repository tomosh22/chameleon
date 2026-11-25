// WP-49 (R-46, ADR-0044): hook narrowing on switch objects the cell's compiler really wrote.
//
// tests/unit/broker/jump_tables/switches.cpp is compiled four times at build time (jump_tables/objects.cmake): /Od and
// /O2 /Ob0, each as written and with JT_SWAPPED (the bodies of `case 1` and `case 2` of jt_dense exchanged: the same code
// bytes, another jump table). The "old module" is a one-section PE this test links from the plain object: the function's
// section is copied to RVA 0x1000 and its relocations are applied as the linker would (ADDR32NB to a place of the same
// section = its RVA, REL32 to __ImageBase = the displacement to the image base). narrow_hooks() then compares
//   * the plain object with that image: identical (dropped) under cl, whose table lives in the function's own code;
//   * the swapped object with it: kept (under cl /Od only the table differs, so only the entry comparison can see it);
//   * both with the record of an earlier patch (WP-25), the way a function hooked by an earlier patch is compared.
// Under clang-cl the table lives in an associative .rdata section: the function stays unverifiable (hooked), as before.
// Each case prints `WP49-OBJECT compiler=... opt=... function=... shape=... identical=... swapped=...`.
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cml/coff/coff.h"
#include "cml/core/hook_narrowing.h"
#include "cml/pe/pe.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
using namespace cml::core::bind;

#ifndef CML_JT_OBJ_DIR
#define CML_JT_OBJ_DIR ""
#endif

namespace {

#ifdef CML_JT_COMPILER_IS_CL
constexpr bool kIsCl = true;
constexpr const char* kCompiler = "cl";
#else
constexpr bool kIsCl = false;
constexpr const char* kCompiler = "clang-cl";
#endif

constexpr std::uint64_t kBase = 0x140000000ull;
constexpr std::uint32_t kTextRva = 0x1000;
constexpr std::uint32_t kTextSize = 0x1000;
constexpr std::uint32_t kTextRaw = 0x200;

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

void w16(std::vector<std::byte>& b, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 2; ++i) {
        b[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}
void w32(std::vector<std::byte>& b, std::size_t at, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i) {
        b[at + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
}
std::uint32_t r32(std::span<const std::byte> b, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[at + i])) << (8 * i);
    }
    return v;
}

// A PE32+ file with one code section `.text` (RVA 0x1000, 0x1000 bytes of int3) holding `code` at its start.
std::vector<std::byte> one_section_image(const std::vector<std::byte>& code) {
    std::vector<std::byte> b(kTextRaw + kTextSize, std::byte{0});
    const std::size_t nt = 0x80;
    const std::size_t opt = nt + 4 + 20;
    w16(b, 0, 0x5A4D);
    w32(b, 0x3C, static_cast<std::uint32_t>(nt));
    w32(b, nt, 0x00004550);
    w16(b, nt + 4, 0x8664);
    w16(b, nt + 6, 1);
    w16(b, nt + 20, 0xF0);
    w16(b, nt + 22, 0x0022);
    w16(b, opt + 0, 0x20B);
    w32(b, opt + 16, kTextRva);
    w32(b, opt + 20, kTextRva);
    w32(b, opt + 24, static_cast<std::uint32_t>(kBase));
    w32(b, opt + 28, static_cast<std::uint32_t>(kBase >> 32));
    w32(b, opt + 32, 0x1000);
    w32(b, opt + 36, 0x200);
    w32(b, opt + 56, kTextRva + kTextSize);
    w32(b, opt + 60, kTextRaw);
    w16(b, opt + 68, 3);
    w32(b, opt + 108, 16);
    const std::size_t sec = opt + 0xF0;
    std::memcpy(b.data() + sec, ".text", 5);
    w32(b, sec + 8, kTextSize);
    w32(b, sec + 12, kTextRva);
    w32(b, sec + 16, kTextSize);
    w32(b, sec + 20, kTextRaw);
    w32(b, sec + 36, 0x60000020);
    for (std::size_t i = 0; i < kTextSize; ++i) {
        b[kTextRaw + i] = i < code.size() ? code[i] : std::byte{0xCC};
    }
    return b;
}

enum class Shape { OwnSection, OtherSection, None };
const char* to_string(Shape s) {
    switch (s) {
        case Shape::OwnSection: return "own-section";
        case Shape::OtherSection: return "rdata";
        case Shape::None: return "none";
    }
    return "?";
}

// One compiled object and the function under test in it.
struct Compiled {
    std::unique_ptr<coff::OwnedObject> owned;
    const coff::Section* sec = nullptr;
    std::uint32_t sec_index = 0;
    std::uint32_t value = 0;
    std::uint32_t len = 0;
    std::uint32_t symbol = 0;  // raw index
    std::vector<coff::ResolvedRelocation> relocs;
    Shape shape = Shape::None;
    std::size_t own_section_refs = 0;
};

Compiled load(const std::string& file, const std::string& function) {
    const std::string path = std::string(CML_JT_OBJ_DIR) + "/" + file;
    Result<coff::OwnedObject> loaded = coff::load_object_file(path);
    REQUIRE_MESSAGE(loaded.has_value(), path << ": " << loaded.error().format());
    Compiled c;
    c.owned = std::make_unique<coff::OwnedObject>(std::move(*loaded));
    const coff::Object& o = c.owned->object;
    for (const coff::Symbol& s : o.symbols()) {
        if (s.name == function && s.section_number > 0) {
            c.sec_index = static_cast<std::uint32_t>(s.section_number);
            c.value = s.value;
            c.symbol = s.index;
        }
    }
    REQUIRE_MESSAGE(c.sec_index != 0, function << " is not defined in " << path);
    c.sec = o.section(c.sec_index);
    REQUIRE(c.sec != nullptr);
    REQUIRE_MESSAGE(c.sec->is_comdat(), "/Gy puts " << function << " in a COMDAT section of its own");
    c.len = static_cast<std::uint32_t>(c.sec->data.size() - c.value);
    REQUIRE(c.len <= kTextSize);
    Result<std::vector<coff::ResolvedRelocation>> relocs = o.resolve_relocations(*c.sec);
    REQUIRE_MESSAGE(relocs.has_value(), relocs.error().format());
    c.relocs = std::move(*relocs);
    bool other = false;
    for (const coff::ResolvedRelocation& r : c.relocs) {
        if (r.target->section_number == static_cast<std::int32_t>(c.sec_index) && r.reloc.type == coff::RelocType::Addr32Nb) {
            ++c.own_section_refs;
        } else if (r.target->section_number > 0 && r.target->section_number != static_cast<std::int32_t>(c.sec_index)) {
            other = true;
        }
    }
    c.shape = c.own_section_refs != 0 ? Shape::OwnSection : (other ? Shape::OtherSection : Shape::None);
    return c;
}

// The old module: the function linked at RVA 0x1000 the way the linker would place it.
std::vector<std::byte> link_old(const Compiled& c) {
    std::vector<std::byte> code(c.sec->data.begin() + c.value, c.sec->data.end());
    for (const coff::ResolvedRelocation& r : c.relocs) {
        const std::uint64_t site = r.reloc.virtual_address;
        if (site < c.value) {
            continue;
        }
        const std::size_t at = static_cast<std::size_t>(site - c.value);
        const std::uint32_t addend = r32(c.sec->data, site);
        const std::string name(r.target->name);
        const bool same_section = r.target->section_number == static_cast<std::int32_t>(c.sec_index);
        if (r.reloc.type == coff::RelocType::Addr32Nb && same_section) {
            w32(code, at, kTextRva + r.target->value + addend - c.value);
        } else if (r.reloc.type >= coff::RelocType::Rel32 && r.reloc.type <= coff::RelocType::Rel32_5 && name == "__ImageBase") {
            const std::uint32_t n = static_cast<std::uint32_t>(r.reloc.type) - static_cast<std::uint32_t>(coff::RelocType::Rel32);
            w32(code, at, addend - (kTextRva + static_cast<std::uint32_t>(at) + 4 + n));
        } else if (r.target->section_number > 0 && !same_section) {
            w32(code, at, 0);  // clang-cl's table in .rdata: never read, the function is not judged
        } else {
            FAIL("unexpected relocation " << coff::reloc_type_name(r.reloc.type) << " to '" << name << "' at +" << at);
        }
    }
    return one_section_image(code);
}

// The plan the planner would make for the function: hooked to its old copy at RVA 0x1000, __ImageBase provided by the linker.
BindPlan plan_for(const Compiled& c, const std::string& function) {
    BindPlan plan;
    SymbolDecision fn;
    fn.symbol_index = c.symbol;
    fn.name = function;
    fn.display = function;
    fn.decision = DecisionKind::PatchHooked;
    fn.defined = true;
    fn.type = SymbolType::Function;
    fn.section = c.sec_index;
    fn.value = c.value;
    fn.old_address.valid = true;
    fn.old_address.va = kBase + kTextRva;
    plan.decisions.push_back(fn);
    for (const coff::ResolvedRelocation& r : c.relocs) {
        if (r.target->name == "__ImageBase") {
            SymbolDecision d;
            d.symbol_index = r.reloc.symbol_index;
            d.name = "__ImageBase";
            d.display = "__ImageBase";
            d.decision = DecisionKind::LinkerProvided;
            plan.decisions.push_back(d);
        }
    }
    HookEntry h;
    h.new_symbol = function;
    h.display = function;
    h.old_va = kBase + kTextRva;
    h.old_rva = kTextRva;
    h.old_size = c.len;
    plan.hooks.push_back(h);
    return plan;
}

struct Verdict {
    NarrowReason reason = NarrowReason::Unchanged;
    std::string detail;
};

Verdict judge(const Compiled& now, const std::string& function, const pe::Image& old, std::shared_ptr<const CodeImage> prior = nullptr) {
    const BindPlan plan = plan_for(now, function);
    NarrowInput in;
    in.object = &now.owned->object;
    in.plan = &plan;
    in.old_image = &old;
    in.load_base = kBase;
    if (prior) {
        in.is_hooked = [](std::uint64_t) { return true; };
        in.prior_code = [prior](std::uint64_t) { return prior; };
    }
    const NarrowResult r = narrow_hooks(in);
    REQUIRE(r.decisions.size() == 1);
    return Verdict{r.decisions[0].reason, r.decisions[0].detail};
}

std::shared_ptr<const CodeImage> record(const Compiled& c, const std::string& function, std::string* why) {
    const BindPlan plan = plan_for(c, function);
    NarrowInput in;
    in.object = &c.owned->object;
    in.plan = &plan;
    in.load_base = kBase;
    std::optional<CodeImage> image = capture_code(in, function, why);
    return image ? std::make_shared<const CodeImage>(std::move(*image)) : nullptr;
}

void run_opt(const char* opt) {
    QuietLogs quiet;
    const std::string function = "jt_dense";
    const Compiled plain = load(std::string("switches_") + opt + "_plain.obj", function);
    const Compiled swapped = load(std::string("switches_") + opt + "_swapped.obj", function);
    const std::vector<std::byte> old_bytes = link_old(plain);
    Result<pe::Image> old = pe::Image::parse(std::span<const std::byte>(old_bytes.data(), old_bytes.size()), pe::ImageKind::OnDisk);
    REQUIRE_MESSAGE(old.has_value(), old.error().format());

    const Verdict same = judge(plain, function, *old);
    const Verdict changed = judge(swapped, function, *old);
    std::string why;
    const std::shared_ptr<const CodeImage> prior = record(plain, function, &why);
    std::printf("WP49-OBJECT compiler=%s opt=%s function=%s bytes=%u relocations=%zu own_section_addr32nb=%zu shape=%s identical=%s swapped=%s (%s) record=%s\n",
                kCompiler, opt, function.c_str(), plain.len, plain.relocs.size(), plain.own_section_refs, to_string(plain.shape), to_string(same.reason),
                to_string(changed.reason), changed.detail.c_str(), prior ? "yes" : ("no: " + why).c_str());
    std::fflush(stdout);
    CAPTURE(same.detail);
    CAPTURE(changed.detail);

    if constexpr (kIsCl) {
        // cl puts the table behind the code, in the function's own section: the WP-49 path is taken (it cannot rot silently).
        REQUIRE_MESSAGE(plain.shape == Shape::OwnSection, "cl " << opt << ": no ADDR32NB to the function's own section");
        CHECK(same.reason == NarrowReason::Unchanged);
        CHECK(changed.reason != NarrowReason::Unchanged);
        if (std::string(opt) == "Od") {
            // Only the jump table differs: the masked bytes are equal, the entries are not.
            CHECK(changed.reason == NarrowReason::TargetChanged);
            CHECK(changed.detail.find("jump-table reference") != std::string::npos);
        }
        REQUIRE_MESSAGE(prior != nullptr, why);
        CHECK(judge(plain, function, *old, prior).reason == NarrowReason::Unchanged);
        CHECK(judge(swapped, function, *old, prior).reason != NarrowReason::Unchanged);
    } else {
        // clang-cl: a table in .rdata keeps the function unverifiable (hooked as before); without a table it is plain code.
        if (plain.shape == Shape::OtherSection) {
            CHECK(same.reason == NarrowReason::Unverifiable);
            CHECK(prior == nullptr);
        } else {
            CHECK(same.reason == NarrowReason::Unchanged);
        }
        CHECK(changed.reason != NarrowReason::Unchanged);
    }

    // A sparse switch (cl may use a byte index table and a jump table): an identical object is dropped or, when a reference
    // is not judged, kept as unverifiable; it is never anything else.
    const Compiled sparse = load(std::string("switches_") + opt + "_plain.obj", "jt_sparse");
    const std::vector<std::byte> sparse_bytes = link_old(sparse);
    Result<pe::Image> sparse_old = pe::Image::parse(std::span<const std::byte>(sparse_bytes.data(), sparse_bytes.size()), pe::ImageKind::OnDisk);
    REQUIRE_MESSAGE(sparse_old.has_value(), sparse_old.error().format());
    const Verdict sparse_same = judge(sparse, "jt_sparse", *sparse_old);
    std::printf("WP49-OBJECT compiler=%s opt=%s function=jt_sparse bytes=%u relocations=%zu own_section_addr32nb=%zu shape=%s identical=%s%s%s\n", kCompiler, opt,
                sparse.len, sparse.relocs.size(), sparse.own_section_refs, to_string(sparse.shape), to_string(sparse_same.reason),
                sparse_same.detail.empty() ? "" : " - ", sparse_same.detail.c_str());
    std::fflush(stdout);
    CHECK_MESSAGE((sparse_same.reason == NarrowReason::Unchanged || sparse_same.reason == NarrowReason::Unverifiable), sparse_same.detail);
}

}  // namespace

TEST_CASE("narrowing (WP-49): switch objects of this compiler at /Od") {
    run_opt("Od");
}

TEST_CASE("narrowing (WP-49): switch objects of this compiler at /O2") {
    run_opt("O2");
}
