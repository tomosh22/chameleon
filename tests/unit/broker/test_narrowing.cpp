// Hook narrowing (WP-12, review item 3): which of the planner's hooks are really needed.
//
// The "old module" is the synthetic PE of tests/unit/pe (its .text is 0xCC filler at RVA 0x1000, .rdata at 0x2000);
// each test writes the old function bytes it wants into that file image, builds the NEW object with the WP-06
// writer (bind tests' SynthObj) and a hand-made BindPlan, and asks narrow_hooks().
#include "cml/core/hook_narrowing.h"

#include <cstring>
#include <string>
#include <vector>

#include "../bind/synth.h"
#include "../pe/pe_builder.h"
#include "cml/util/log.h"
#include "doctest.h"

using namespace cml;
using namespace cml::core;
using namespace cml::core::bind;
using namespace cml_bind_test;

namespace {

constexpr std::uint64_t kPreferred = 0x140000000ull;
constexpr std::uint32_t kFnRva = 0x1100;     // the old function (16 bytes)
constexpr std::uint32_t kBarRva = 0x1200;    // an old function the code calls
constexpr std::uint32_t kConstRva = 0x2980;  // an old constant in .rdata

struct QuietLogs {
    LogLevel previous = log_level();
    QuietLogs() { set_log_level(LogLevel::Off); }
    ~QuietLogs() { set_log_level(previous); }
};

std::size_t text_off(std::uint32_t rva) { return cml_test::pe_layout::kTextRaw + (rva - cml_test::pe_layout::kTextRva); }

void put32(cml_test::PeBytes& b, std::size_t off, std::uint32_t v) { cml_test::w32(b, off, v); }

// One scenario: an old image, a new object with one function `foo`, and the plan describing them.
struct Scenario {
    cml_test::PeBytes disk = cml_test::build_pe_on_disk();
    SynthObj obj;
    BindPlan plan;
    std::uint64_t load_base = kPreferred;
    SynthObj::Func foo;

    Scenario() { foo = obj.function("?foo@@YAHXZ", true, 16); }

    void old_code(const std::vector<std::uint8_t>& bytes) {
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            disk[text_off(kFnRva) + i] = bytes[i];
        }
    }
    void new_code(const std::vector<std::uint8_t>& bytes) {
        coff::ModelSection& s = obj.model().sections[foo.section - 1];
        for (std::byte& b : s.data) {
            b = std::byte{0xCC};  // the old image's filler
        }
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            s.data[i] = static_cast<std::byte>(bytes[i]);
        }
    }
    SymbolDecision& decision(std::uint32_t raw_index, const std::string& name, DecisionKind kind) {
        SymbolDecision d;
        d.symbol_index = raw_index;
        d.name = name;
        d.display = name;
        d.decision = kind;
        plan.decisions.push_back(d);
        return plan.decisions.back();
    }
    void hook(std::uint32_t old_size = 16, std::uint32_t old_rva = kFnRva) {
        SymbolDecision& d = decision(foo.symbol, "?foo@@YAHXZ", DecisionKind::PatchHooked);
        d.defined = true;
        d.type = SymbolType::Function;
        d.section = foo.section;
        d.value = 0;
        d.old_address.valid = true;
        d.old_address.va = load_base + old_rva;
        HookEntry h;
        h.new_symbol = "?foo@@YAHXZ";
        h.display = "foo";
        h.old_va = load_base + old_rva;
        h.old_rva = old_rva;
        h.old_size = old_size;
        plan.hooks.push_back(h);
    }
    NarrowResult run(bool enabled = true, std::function<bool(std::uint64_t)> hooked = {}) {
        Built built = obj.build();
        keep = std::move(built);
        auto image = pe::Image::parse(cml_test::pe_span(disk), pe::ImageKind::OnDisk);
        REQUIRE_MESSAGE(image.has_value(), image.error().format());
        NarrowInput in;
        in.object = &keep.obj();
        in.plan = &plan;
        in.old_image = &*image;
        in.load_base = load_base;
        in.is_hooked = std::move(hooked);
        in.enabled = enabled;
        return narrow_hooks(in);
    }
    Built keep;
};

}  // namespace

TEST_CASE("narrowing: identical code and identical call target is dropped") {
    QuietLogs quiet;
    Scenario s;
    // call bar; ret.  The old displacement points at bar; the new object relocates against bar.
    s.old_code({0xE8, 0, 0, 0, 0, 0xC3});
    const std::int32_t disp = static_cast<std::int32_t>(kBarRva) - static_cast<std::int32_t>(kFnRva + 1 + 4);
    put32(s.disk, text_off(kFnRva) + 1, static_cast<std::uint32_t>(disp));
    s.new_code({0xE8, 0, 0, 0, 0, 0xC3});
    const std::uint32_t bar = s.obj.undefined("bar", true);
    s.obj.reloc(s.foo.section, 1, bar, kRel32, 0);
    s.hook();
    SymbolDecision& d = s.decision(bar, "bar", DecisionKind::BindOldFunction);
    d.old_address.valid = true;
    d.old_address.va = kPreferred + kBarRva;
    const NarrowResult r = s.run();
    REQUIRE(r.decisions.size() == 1);
    CHECK_MESSAGE(r.decisions[0].reason == NarrowReason::Unchanged, to_string(r.decisions[0].reason) << " " << r.decisions[0].detail);
    CHECK(r.hooks.empty());
    CHECK(r.dropped == 1);
}

TEST_CASE("narrowing: a different instruction byte keeps the hook") {
    QuietLogs quiet;
    Scenario s;
    s.old_code({0xB8, 1, 0, 0, 0, 0xC3});  // mov eax, 1 ; ret
    s.new_code({0xB8, 2, 0, 0, 0, 0xC3});  // mov eax, 2 ; ret
    s.hook();
    const NarrowResult r = s.run();
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::BytesDiffer);
    CHECK(r.decisions[0].detail.find("byte 1") != std::string::npos);
}

TEST_CASE("narrowing: identical bytes are dropped even when the function is all instructions") {
    QuietLogs quiet;
    Scenario s;
    s.old_code({0xB8, 1, 0, 0, 0, 0xC3});
    s.new_code({0xB8, 1, 0, 0, 0, 0xC3});
    s.hook(16);
    CHECK(s.run().hooks.empty());
}

TEST_CASE("narrowing: a call that now goes to another function keeps the hook (masked bytes alone would miss it)") {
    QuietLogs quiet;
    Scenario s;
    s.old_code({0xE8, 0, 0, 0, 0, 0xC3});
    const std::int32_t disp = static_cast<std::int32_t>(kBarRva) - static_cast<std::int32_t>(kFnRva + 1 + 4);
    put32(s.disk, text_off(kFnRva) + 1, static_cast<std::uint32_t>(disp));
    s.new_code({0xE8, 0, 0, 0, 0, 0xC3});
    const std::uint32_t baz = s.obj.undefined("baz", true);
    s.obj.reloc(s.foo.section, 1, baz, kRel32, 0);
    s.hook();
    SymbolDecision& d = s.decision(baz, "baz", DecisionKind::BindOldFunction);
    d.old_address.valid = true;
    d.old_address.va = kPreferred + 0x1240;  // not what the old code called
    const NarrowResult r = s.run();
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);
}

TEST_CASE("narrowing: the in-place addend is part of the target (REL32 with A = -1, clang style)") {
    QuietLogs quiet;
    Scenario s;
    // cmp dword ptr [rip+x], 0 : 83 3D <rel32> 00. clang's REL32 carries A = -1; the old displacement is relative to
    // the end of the instruction, i.e. 1 byte after the field.
    s.old_code({0x83, 0x3D, 0, 0, 0, 0, 0x00, 0xC3});
    const std::uint32_t g_rva = 0x2980;
    const std::int32_t disp = static_cast<std::int32_t>(g_rva) - static_cast<std::int32_t>(kFnRva + 2 + 4 + 1);
    put32(s.disk, text_off(kFnRva) + 2, static_cast<std::uint32_t>(disp));
    s.new_code({0x83, 0x3D, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xC3});  // A = -1 stored in place
    const std::uint32_t g = s.obj.undefined("g_tick", false);
    s.obj.reloc(s.foo.section, 2, g, kRel32, -1);
    s.hook();
    SymbolDecision& d = s.decision(g, "g_tick", DecisionKind::BindOldData);
    d.old_address.valid = true;
    d.old_address.va = kPreferred + g_rva;
    const NarrowResult r = s.run();
    CHECK_MESSAGE(r.decisions[0].reason == NarrowReason::Unchanged, to_string(r.decisions[0].reason) << " " << r.decisions[0].detail);
}

TEST_CASE("narrowing: a reference to a symbol that does not exist in the old image keeps the hook") {
    QuietLogs quiet;
    Scenario s;
    s.old_code({0xE8, 0, 0, 0, 0, 0xC3});
    s.new_code({0xE8, 0, 0, 0, 0, 0xC3});
    const std::uint32_t helper = s.obj.undefined("new_helper", true);
    s.obj.reloc(s.foo.section, 1, helper, kRel32, 0);
    s.hook();
    s.decision(helper, "new_helper", DecisionKind::PatchNewFunction);
    const NarrowResult r = s.run();
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::TargetNew);
}

TEST_CASE("narrowing: size, unknown size, missing old bytes, already hooked, disabled") {
    QuietLogs quiet;
    SUBCASE("the new function is longer") {
        Scenario s;
        s.old_code({0xC3});
        s.hook(8);  // old size 8, new section 16 bytes
        CHECK(s.run().decisions[0].reason == NarrowReason::SizeDiffers);
    }
    SUBCASE("the PDB gave no size") {
        Scenario s;
        s.hook(0);
        CHECK(s.run().decisions[0].reason == NarrowReason::UnknownOldSize);
    }
    SUBCASE("old entry not in the module file (defined by an earlier patch)") {
        Scenario s;
        s.hook(16, bind::kNoRva);
        CHECK(s.run().decisions[0].reason == NarrowReason::OldBytesUnavailable);
    }
    SUBCASE("old bytes outside the file's sections") {
        Scenario s;
        s.hook(16, 0x9000);
        CHECK(s.run().decisions[0].reason == NarrowReason::OldBytesUnavailable);
    }
    SUBCASE("an entry that currently redirects to an earlier patch is always kept, even when the code equals the original") {
        Scenario s;
        s.old_code({0xC3});
        s.new_code({0xC3});
        s.hook();
        const NarrowResult r = s.run(true, [](std::uint64_t va) { return va == kPreferred + kFnRva; });
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::AlreadyHooked);
    }
    SUBCASE("narrowing switched off hooks everything") {
        Scenario s;
        s.old_code({0xC3});
        s.new_code({0xC3});
        s.hook();
        const NarrowResult r = s.run(false);
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::Disabled);
    }
}

TEST_CASE("narrowing: a string literal or table the patch carries as a fresh copy is compared by content") {
    QuietLogs quiet;
    const auto build = [](bool same, Scenario& s) {
        // lea rax, [rip+literal]; ret
        s.old_code({0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3});
        const std::int32_t disp = static_cast<std::int32_t>(kConstRva) - static_cast<std::int32_t>(kFnRva + 3 + 4);
        put32(s.disk, text_off(kFnRva) + 3, static_cast<std::uint32_t>(disp));
        const char old_text[8] = {'h', 'e', 'l', 'l', 'o', 0, 0, 0};
        std::memcpy(s.disk.data() + cml_test::rdata_off(kConstRva), old_text, 8);
        s.new_code({0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3});
        const std::uint32_t sec = s.obj.section(".rdata$r", kRdata | coff::scn::LnkComdat, 8, coff::ComdatSelection::Any);
        const std::uint32_t lit = s.obj.symbol("??_C@_05literal@", 0, static_cast<std::int32_t>(sec), false, true);
        coff::ModelSection& ms = s.obj.model().sections[sec - 1];
        const char new_text[8] = {'h', 'e', 'l', same ? 'l' : 'p', 'o', 0, 0, 0};
        for (int i = 0; i < 8; ++i) {
            ms.data[static_cast<std::size_t>(i)] = static_cast<std::byte>(new_text[i]);
        }
        s.obj.reloc(s.foo.section, 3, lit, kRel32, 0);
        s.hook();
        SymbolDecision& d = s.decision(lit, "??_C@_05literal@", DecisionKind::PatchFresh);
        d.defined = true;
        d.type = SymbolType::Data;
        d.section = sec;
        d.value = 0;
    };
    SUBCASE("same literal") {
        Scenario s;
        build(true, s);
        const NarrowResult r = s.run();
        CHECK_MESSAGE(r.decisions[0].reason == NarrowReason::Unchanged, to_string(r.decisions[0].reason) << " " << r.decisions[0].detail);
    }
    SUBCASE("edited literal") {
        Scenario s;
        build(false, s);
        const NarrowResult r = s.run();
        REQUIRE(r.hooks.size() == 1);
        CHECK(r.decisions[0].reason == NarrowReason::TargetChanged);
        CHECK(r.decisions[0].detail.find("differs") != std::string::npos);
    }
}

TEST_CASE("narrowing: ADDR64 and SECREL sites are judged against the planned values; load base differs from the preferred one") {
    QuietLogs quiet;
    SUBCASE("ADDR64 to old data, loaded at another base") {
        Scenario s;
        s.load_base = 0x7FF600000000ull;
        s.old_code({0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3});  // movabs rax, imm64
        // The file holds the value for the PREFERRED base; at run time the loader adds (load - preferred).
        const std::uint64_t data_rva = 0x3000;
        const std::uint64_t file_value = kPreferred + data_rva;
        for (int i = 0; i < 8; ++i) {
            s.disk[text_off(kFnRva) + 2 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(file_value >> (8 * i));
        }
        s.new_code({0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3});
        const std::uint32_t g = s.obj.undefined("g_data", false);
        s.obj.reloc(s.foo.section, 2, g, kAddr64, 0);
        s.hook();
        SymbolDecision& d = s.decision(g, "g_data", DecisionKind::BindOldData);
        d.old_address.valid = true;
        d.old_address.va = s.load_base + data_rva;
        CHECK(s.run().hooks.empty());
        d.old_address.va += 8;  // now it points elsewhere
        CHECK(s.run().hooks.size() == 1);
    }
    SUBCASE("SECREL to the thread-safe-static epoch") {
        Scenario s;
        s.old_code({0x8B, 0x82, 0, 0, 0, 0, 0xC3});  // mov eax, [rdx+secrel]
        put32(s.disk, text_off(kFnRva) + 2, 0x24);
        s.new_code({0x8B, 0x82, 0, 0, 0, 0, 0xC3});
        const std::uint32_t epoch = s.obj.undefined("_Init_thread_epoch", false);
        s.obj.reloc(s.foo.section, 2, epoch, coff::RelocType::SecRel, 0);
        s.hook();
        SymbolDecision& d = s.decision(epoch, "_Init_thread_epoch", DecisionKind::BindOldData);
        d.old_address.valid = true;
        d.old_address.va = kPreferred + 0x5000;
        Fixup f;
        f.section_index = s.foo.section;
        f.offset = 2;
        f.kind = FixupKind::SecRel32;
        f.target_section_offset = 0x24;
        f.addend = 0;
        s.plan.fixups.push_back(f);
        CHECK(s.run().hooks.empty());
        s.plan.fixups[0].target_section_offset = 0x28;
        CHECK(s.run().hooks.size() == 1);
    }
}

TEST_CASE("narrowing: a constant that holds pointers (a vtable) is never judged equal") {
    QuietLogs quiet;
    Scenario s;
    s.old_code({0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3});
    const std::int32_t disp = static_cast<std::int32_t>(kConstRva) - static_cast<std::int32_t>(kFnRva + 3 + 4);
    put32(s.disk, text_off(kFnRva) + 3, static_cast<std::uint32_t>(disp));
    s.new_code({0x48, 0x8D, 0x05, 0, 0, 0, 0, 0xC3});
    const std::uint32_t sec = s.obj.section(".rdata", kRdata | coff::scn::LnkComdat, 16, coff::ComdatSelection::Any);
    const std::uint32_t vt = s.obj.symbol("??_7Foo@@6B@", 0, static_cast<std::int32_t>(sec), false, true);
    const std::uint32_t target = s.obj.undefined("Foo::f", true);
    s.obj.reloc(sec, 8, target, kAddr64, 0);
    s.obj.reloc(s.foo.section, 3, vt, kRel32, 0);
    s.hook();
    SymbolDecision& d = s.decision(vt, "??_7Foo@@6B@", DecisionKind::PatchFresh);
    d.defined = true;
    d.type = SymbolType::Data;
    d.section = sec;
    const NarrowResult r = s.run();
    REQUIRE(r.hooks.size() == 1);
    CHECK(r.decisions[0].reason == NarrowReason::Unverifiable);
}
